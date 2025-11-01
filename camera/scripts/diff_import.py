#!/usr/bin/env python3
import argparse
import sys
import subprocess
import difflib
from pathlib import Path
from collections import defaultdict

DEFAULT_THRESHOLD = 0.9

def get_clipboard_content():
    """Retrieves content from the system clipboard."""
    # (Unchanged)
    try:
        if sys.platform == "darwin":
            command = ["pbpaste"]
        elif sys.platform.startswith("linux"):
            command = ["xclip", "-selection", "clipboard", "-o"]
        else:
            print(f"Error: --paste is not supported on this platform ({sys.platform}).", file=sys.stderr)
            return None
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        return result.stdout
    except FileNotFoundError:
        print(f"Error: Command '{command[0]}' not found. Is it installed and in your PATH?", file=sys.stderr)
        print("On Debian/Ubuntu, try: sudo apt install xclip", file=sys.stderr)
        return None
    except subprocess.CalledProcessError as e:
        print(f"Error: Failed to get clipboard content using '{command[0]}'.", file=sys.stderr)
        print(f"Stderr: {e.stderr}", file=sys.stderr)
        return None

def parse_diff_file(diff_content):
    """
    Parses diff content in a special format, handling multiple variations.

    - Standard diff block with original and new content.
    - Full file content block (no '#=====' separator).

    Format 1 (single code block):
    #>>>>> FILENAME
    ```
    ORIGINAL
    #=====
    NEW
    ```
    #<<<<< end

    Format 2 (separate code blocks):
    #>>>>> FILENAME
    ```
    ORIGINAL
    ```
    #=====
    ```
    NEW
    ```
    #<<<<< end

    Format 3 (full file replacement/creation, no '#====='):
    #>>>>> FILENAME
    ```
    NEW
    ```
    #<<<<< end
    """
    lines = diff_content.splitlines(keepends=True)
    state = "SEEK_START"
    patch = {}
    line_num = 0

    for line in lines:
        line_num += 1
        stripped_line = line.strip()

        if state == "SEEK_START":
            if stripped_line.startswith("#>>>>>"):
                patch = {'filename': stripped_line[6:].strip(), 'original': [], 'new': []}
                state = "EXPECT_ORIGINAL_OPEN_BACKTICK"

        elif state == "EXPECT_ORIGINAL_OPEN_BACKTICK":
            if stripped_line == "```":
                state = "IN_ORIGINAL"
            elif stripped_line:
                print(f"Error: Malformed patch on line {line_num}. Expected '```' after '#>>>>>' line. Skipping block.", file=sys.stderr)
                state = "SEEK_START"

        elif state == "IN_ORIGINAL":
            if stripped_line == "#=====":  # Transition for Format 1
                state = "IN_NEW"
            elif stripped_line == "```":  # Transition for Format 2
                state = "EXPECT_SEPARATOR"
            else:
                patch['original'].append(line)

        elif state == "EXPECT_SEPARATOR":
            if stripped_line == "#=====":
                state = "EXPECT_NEW_OPEN_BACKTICK"
            elif stripped_line == "#<<<<< end":
                # This is a full file replacement/creation block (Format 3).
                # The content we parsed as 'original' is actually 'new'.
                patch['new'] = patch['original']
                patch['original'] = []
                patch['original'] = "".join(patch['original'])
                patch['new'] = "".join(patch['new'])
                yield patch
                patch = {}
                state = "SEEK_START"
            elif stripped_line:
                print(f"Error: Malformed patch on line {line_num}. Expected '#=====' or '#<<<<< end' after closing '```'. Skipping block.", file=sys.stderr)
                state = "SEEK_START"

        elif state == "EXPECT_NEW_OPEN_BACKTICK":
            if stripped_line == "```":
                state = "IN_NEW"
            elif stripped_line:
                print(f"Error: Malformed patch on line {line_num}. Expected '```' after '#====='. Skipping block.", file=sys.stderr)
                state = "SEEK_START"

        elif state == "IN_NEW":
            if stripped_line == "```":
                state = "EXPECT_END_MARKER"
            else:
                patch['new'].append(line)

        elif state == "EXPECT_END_MARKER":
            if stripped_line == "#<<<<< end":
                patch['original'] = "".join(patch['original'])
                patch['new'] = "".join(patch['new'])
                yield patch
                patch = {}  # Reset for next iteration
                state = "SEEK_START"
            elif stripped_line:
                print(f"Error: Malformed patch on line {line_num}. Expected '#<<<<< end' after closing '```'. Skipping block.", file=sys.stderr)
                state = "SEEK_START"

    if state not in ["SEEK_START"]:
        print(f"Error: Diff content ended unexpectedly. Check for missing markers or '```'. Final state: {state}", file=sys.stderr)


def find_patch_location(file_content, original_text, threshold):
    """
    Finds the best location for a patch in the given file content.
    Returns a tuple: (start_index, end_index, match_method, reason) or (None, None, None, reason).
    """
    # (Slightly modified to return a reason for failure)
    if not original_text.strip():
        # This case is now handled by `apply_collated_patches` for creation/replacement.
        return None, None, None, "Original block is empty."

    # STAGE 1: Verbatim Match
    start_index = file_content.find(original_text)
    if start_index != -1:
        if file_content.find(original_text, start_index + 1) == -1:
            end_index = start_index + len(original_text)
            return start_index, end_index, "verbatim", "Success"
        else:
            return None, None, None, "Ambiguous: found multiple verbatim matches."

    # STAGE 2: Fuzzy Match
    best_ratio = -1.0; best_match_start = -1; best_match_end = -1
    chunk_size = int(len(original_text) * 1.5); step_size = max(1, int(chunk_size / 3))

    for i in range(0, len(file_content) - chunk_size + 1, step_size):
        chunk = file_content[i : i + chunk_size]
        sm = difflib.SequenceMatcher(a=chunk, b=original_text, autojunk=False)
        ratio = sm.ratio()
        if ratio > best_ratio:
            best_ratio = ratio
            opcodes = sm.get_opcodes()
            start_in_chunk = opcodes[0][1] if opcodes else 0
            end_in_chunk = opcodes[-1][2] if opcodes else 0
            best_match_start = i + start_in_chunk
            best_match_end = i + end_in_chunk

    if best_ratio >= threshold:
        return best_match_start, best_match_end, f"fuzzy ({best_ratio:.2f})", "Success"
    else:
        reason = f"Best fuzzy match ({best_ratio:.2f}) is below threshold ({threshold})."
        return None, None, None, reason

def apply_collated_patches(filename, patches, threshold, dry_run=False):
    """
    Applies patches to a single file, returning a detailed report for each patch.
    Handles file creation, full file replacement, and targeted modifications.
    """
    target_file = Path(filename)
    patch_results = [{'status': 'pending', 'patch': p} for p in patches]

    is_creation_or_replacement_patch = lambda p: not p['original'].strip()

    # --- File Creation Logic ---
    if not target_file.exists():
        if all(is_creation_or_replacement_patch(p['patch']) for p in patch_results):
            full_new_content = "".join(p['patch']['new'] for p in patch_results)
            if dry_run:
                print(f"\n--- DRY RUN: Would create new file {filename}")
            else:
                try:
                    target_file.parent.mkdir(parents=True, exist_ok=True)
                    target_file.write_text(full_new_content, encoding='utf-8')
                except Exception as e:
                    for result in patch_results:
                        result['status'] = 'failed_to_create'
                        result['reason'] = str(e)
                    return patch_results
            for result in patch_results:
                result['status'] = 'applied_creation'
            return patch_results
        else:
            # Can't apply a modification patch to a non-existent file.
            for result in patch_results:
                result['status'] = 'failed_to_locate'
                result['reason'] = "File not found and patch is for modification."
            return patch_results

    # --- File Exists: Handle Replacement or Modification ---
    try:
        original_content = target_file.read_text(encoding='utf-8')
    except Exception as e:
        for result in patch_results:
            result['status'] = 'failed_to_read'
            result['reason'] = str(e)
        return patch_results

    # Check for full file replacement scenario on an existing file.
    replacement_patches = [res for res in patch_results if is_creation_or_replacement_patch(res['patch'])]
    if replacement_patches:
        if len(patch_results) > 1:
            # Cannot mix a full file replacement with other patch types for the same file.
            for res in patch_results:
                res['status'] = 'failed_ambiguous'
                res['reason'] = 'A full file replacement patch cannot be combined with other modification patches for the same file.'
            return patch_results

        # Proceed with full replacement.
        replacement_result = replacement_patches[0]
        new_content = replacement_result['patch']['new']
        if dry_run:
            print(f"\n--- DRY RUN: Would replace the entire content of {filename}")
            diff = difflib.unified_diff(original_content.splitlines(keepends=True), new_content.splitlines(keepends=True), fromfile=f"a/{filename}", tofile=f"b/{filename}")
            sys.stdout.writelines(diff)
        else:
            try:
                target_file.write_text(new_content, encoding='utf-8')
            except Exception as e:
                replacement_result['status'] = 'failed_to_write'
                replacement_result['reason'] = str(e)
                return patch_results
        replacement_result['status'] = 'applied_replacement'
        return patch_results

    # --- Standard File Modification Logic ---
    located_patches = []
    for result in patch_results:
        start, end, method, reason = find_patch_location(original_content, result['patch']['original'], threshold)
        if start is not None:
            result['start'], result['end'], result['method'] = start, end, method
            located_patches.append(result)
        else:
            result['status'] = 'failed_to_locate'
            result['reason'] = reason

    if not located_patches:
        return patch_results

    located_patches.sort(key=lambda p: p['start'])
    has_conflict = False
    for i in range(len(located_patches) - 1):
        if located_patches[i]['end'] > located_patches[i+1]['start']:
            has_conflict = True
            located_patches[i]['status'] = 'skipped_due_to_conflict'
            located_patches[i+1]['status'] = 'skipped_due_to_conflict'
            located_patches[i]['reason'] = f"Overlaps with patch at [{located_patches[i+1]['start']}:{located_patches[i+1]['end']}]"
            located_patches[i+1]['reason'] = f"Overlaps with patch at [{located_patches[i]['start']}:{located_patches[i]['end']}]"

    if has_conflict:
        return patch_results

    modified_content = original_content
    for patch_info in sorted(located_patches, key=lambda p: p['start'], reverse=True):
        start, end, new_text = patch_info['start'], patch_info['end'], patch_info['patch']['new']
        modified_content = modified_content[:start] + new_text + modified_content[end:]
        patch_info['status'] = 'applied_modification'

    if dry_run:
        print(f"\n--- DRY RUN: Changes for {filename} ---")
        diff = difflib.unified_diff(original_content.splitlines(keepends=True), modified_content.splitlines(keepends=True), fromfile=f"a/{filename}", tofile=f"b/{filename}")
        sys.stdout.writelines(diff)
    else:
        try:
            target_file.write_text(modified_content, encoding='utf-8')
        except Exception as e:
            for p in located_patches:
                p['status'] = 'failed_to_write'
                p['reason'] = str(e)

    return patch_results

def main():
    parser = argparse.ArgumentParser(description="Apply a special fuzzy diff. Reads from a file, --paste, or stdin.", formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("diff_file", nargs='?', help="Path to the diff file. If omitted, reads from stdin or --paste.")
    parser.add_argument("--paste", action="store_true", help="Read diff content from the system clipboard (uses pbpaste or xclip).")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD, help=f"Minimum similarity ratio (0.0 to 1.0) for fuzzy matching (default: {DEFAULT_THRESHOLD}).")
    parser.add_argument("--dry-run", action="store_true", help="Print what would be changed without modifying any files.")
    args = parser.parse_args()

    diff_content = None
    if args.paste: diff_content = get_clipboard_content()
    elif args.diff_file: diff_content = Path(args.diff_file).read_text(encoding='utf-8')
    elif not sys.stdin.isatty(): diff_content = sys.stdin.read()

    if diff_content is None:
        parser.print_help()
        print("\nError: No diff content provided.", file=sys.stderr); sys.exit(1)

    patches_by_file = defaultdict(list)
    total_patches = 0
    for patch in parse_diff_file(diff_content):
        patches_by_file[patch['filename']].append(patch)
        total_patches += 1

    if not total_patches:
        print("No valid patch blocks found in the input."); sys.exit(0)

    print(f"Found {total_patches} patch blocks across {len(patches_by_file)} file(s).")

    total_applied = 0
    for filename, patches in sorted(patches_by_file.items()):
        results = apply_collated_patches(filename, patches, args.threshold, args.dry_run)

        print(f"\n--- Summary for {filename} ---")
        file_applied_count = 0
        for i, res in enumerate(results):
            status = res['status']
            first_line_orig = res['patch']['original'].strip().splitlines()
            context = f'"{first_line_orig[0]}..."' if first_line_orig else "(full file content)"

            if status.startswith('applied'):
                file_applied_count += 1
                if status == 'applied_creation':
                    print(f"  [✓] Chunk #{i+1}: CREATED file with new content.")
                elif status == 'applied_replacement':
                    print(f"  [✓] Chunk #{i+1}: REPLACED file content.")
                else: # applied_modification
                    print(f"  [✓] Chunk #{i+1}: APPLIED patch for {context} via {res['method']} match.")
            else:
                reason = res.get('reason', 'Unknown error')
                # Just use an [x] instead of the prettier unicode char, makes
                # it easier to search.
                print(f"  [x] Chunk #{i+1}: FAILED for {context}. Reason: {reason}")

        total_applied += file_applied_count
        if file_applied_count == len(results):
             print(f"Result: Successfully applied all {len(results)} chunks.")
        else:
             print(f"Result: Applied {file_applied_count} of {len(results)} chunks.")

    print("\n" + "="*20 + " Final Summary " + "="*20)
    print(f"Successfully applied {total_applied} out of {total_patches} total patch blocks.")

    if total_applied < total_patches:
        sys.exit(1)

if __name__ == "__main__":
    main()
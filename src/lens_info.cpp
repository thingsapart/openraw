#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <memory>

#include "lensfun/lensfun.h"

// --- C++ RAII Wrappers for Lensfun C API ---

// Custom deleter for the lfDatabase object.
struct LfDatabaseDeleter {
    void operator()(lfDatabase* db) const {
        if (db) {
            lf_db_destroy(db);
        }
    }
};
using LfDatabasePtr = std::unique_ptr<lfDatabase, LfDatabaseDeleter>;

// Custom deleter for memory allocated by lensfun that needs lf_free().
// This is used for the arrays returned by the lf_db_find_* functions.
struct LfFreeDeleter {
    void operator()(void* ptr) const {
        if (ptr) {
            lf_free(ptr);
        }
    }
};
// A template alias to make using this deleter cleaner.
template<typename T>
using LfScopedPtr = std::unique_ptr<T, LfFreeDeleter>;


void print_usage() {
    std::cout << "Usage: ./lens_info <command> [options]\n\n"
              << "A utility to explore the Lensfun database.\n\n"
              << "Commands:\n"
              << "  --list-makes                    List all unique camera manufacturers.\n"
              << "  --list-cameras [--make <name>]  List all camera models, optionally filtered by manufacturer.\n"
              << "  --list-lenses                   List all lenses in the database.\n"
              << "  --list-lenses --camera-make <make> --camera-model <model>\n"
              << "                                  List lenses compatible with a specific camera.\n\n"
              << "Examples:\n"
              << "  ./lens_info --list-makes\n"
              << "  ./lens_info --list-cameras --make \"Sony\"\n"
              << "  ./lens_info --list-lenses --camera-make \"Raspberry Pi\" --camera-model \"High Quality Camera\"\n"
              << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::map<std::string, std::string> args;
    std::set<std::string> flags;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 < argc && argv[i+1][0] != '-') {
                args[arg.substr(2)] = argv[++i];
            } else {
                flags.insert(arg.substr(2));
            }
        }
    }

    // Initialize the Lensfun database using our RAII wrapper.
    LfDatabasePtr ldb(lf_db_new());
    if (!ldb) {
        std::cerr << "Error: Could not create Lensfun database object." << std::endl;
        return 1;
    }
    if (lf_db_load(ldb.get()) != LF_NO_ERROR) {
        std::cerr << "Error: Could not load data into the Lensfun database." << std::endl;
        return 1;
    }

    if (flags.count("list-makes")) {
        // lf_db_get_cameras returns a database-owned array. Do NOT free it.
        const lfCamera *const *cameras = lf_db_get_cameras(ldb.get());
        if (!cameras) {
            std::cerr << "No cameras found in database." << std::endl;
            return 1;
        }
        std::set<std::string> makes;
        for (int i = 0; cameras[i]; i++) {
            makes.insert(lf_mlstr_get(cameras[i]->Maker));
        }
        std::cout << "--- Camera Manufacturers ---\n";
        for (const auto& make : makes) {
            std::cout << "\"" << make << "\"" << std::endl;
        }

    } else if (flags.count("list-cameras")) {
        const lfCamera *const *cameras = lf_db_get_cameras(ldb.get());
        if (!cameras) {
            std::cerr << "No cameras found in database." << std::endl;
            return 1;
        }
        std::string filter_make = args.count("make") ? args["make"] : "";
        std::cout << "--- Camera Models " << (filter_make.empty() ? "" : "(filtered by Make: \"" + filter_make + "\")") << " ---\n";
        for (int i = 0; cameras[i]; i++) {
            std::string current_make = lf_mlstr_get(cameras[i]->Maker);
            if (filter_make.empty() || current_make == filter_make) {
                std::cout << "  Make: \"" << current_make << "\", Model: \"" << lf_mlstr_get(cameras[i]->Model) << "\"" << std::endl;
            }
        }

    } else if (flags.count("list-lenses")) {
        if (args.count("camera-make") && args.count("camera-model")) {
            // Find a specific camera. lf_db_find_cameras returns a NEWLY ALLOCATED array.
            LfScopedPtr<const lfCamera*> cams((const lfCamera**)lf_db_find_cameras(ldb.get(), args["camera-make"].c_str(), args["camera-model"].c_str()));
            if (!cams || !cams.get()[0]) {
                std::cerr << "Error: Camera not found in Lensfun database." << std::endl;
            } else {
                const lfCamera* cam = cams.get()[0]; // Use the first match
                // Find compatible lenses. lf_db_find_lenses_hd also returns a NEWLY ALLOCATED array.
                // We search with NULL for lens maker/model to find all compatible lenses.
                LfScopedPtr<const lfLens*> lenses((const lfLens**)lf_db_find_lenses_hd(ldb.get(), cam, nullptr, nullptr, 0));

                if (!lenses || !lenses.get()[0]) {
                    std::cout << "No specific lens profiles found for this camera." << std::endl;
                } else {
                    std::cout << "--- Lens Profiles for " << args["camera-make"] << " " << args["camera-model"] << " ---\n";
                    for (int i = 0; lenses.get()[i]; i++) {
                        std::cout << "  \"" << lf_mlstr_get(lenses.get()[i]->Model) << "\"" << std::endl;
                    }
                    std::cout << "\n(Copy and paste one of the model names above into the --lensfun argument for the 'process' tool)" << std::endl;
                }
            }
        } else {
            // List all lenses in the database. lf_db_get_lenses returns a database-owned array. Do NOT free it.
            const lfLens *const *lenses = lf_db_get_lenses(ldb.get());
            if (!lenses) {
                std::cerr << "No lenses found in database." << std::endl;
            } else {
                std::cout << "--- All Lenses in Database ---\n";
                for (int i = 0; lenses[i]; i++) {
                     std::cout << "  Make: \"" << lf_mlstr_get(lenses[i]->Maker)
                               << "\", Model: \"" << lf_mlstr_get(lenses[i]->Model) << "\"" << std::endl;
                }
            }
        }
    } else {
        print_usage();
    }

    // All resources are automatically cleaned up when ldb goes out of scope.
    return 0;
}

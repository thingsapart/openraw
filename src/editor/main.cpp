#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <set>
#include <tuple>

#include "imgui.h"
#include "imgui_internal.h" // For accessing internal context state
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h> 
#include <libraw/libraw.h>

#include "process_options.h"
#include "app_state.h"
#include "editor_theme.h"
#include "editor_ui.h"
#include "halide_runner.h"
#include "texture_utils.h"
#include "halide_image_io.h"
#include "tone_curve_utils.h"


// --- LibRaw Loading (Copied from process.cpp) ---
std::tuple<Halide::Runtime::Buffer<uint16_t, 2>, std::shared_ptr<LibRaw>>
load_raw_file_nocopy_ui(const std::string &path, int &cfa_pattern, int &black_level, int &white_level) {
    auto processor = std::make_shared<LibRaw>();
    processor->imgdata.params.output_bps = 16;
    processor->imgdata.params.use_camera_wb = 0;
    processor->imgdata.params.no_auto_bright = 1;

    if (processor->open_file(path.c_str()) != LIBRAW_SUCCESS) {
        throw std::runtime_error("LibRaw Error: Cannot open file " + path);
    }
    if (processor->unpack() != LIBRAW_SUCCESS) {
        throw std::runtime_error("LibRaw Error: Cannot unpack file " + path);
    }

    int width = processor->imgdata.sizes.width;
    int height = processor->imgdata.sizes.height;
    int pitch = processor->imgdata.sizes.raw_pitch;

    if (processor->imgdata.color.cblack[0] > 0) {
        black_level = (processor->imgdata.color.cblack[0] + processor->imgdata.color.cblack[1] +
                       processor->imgdata.color.cblack[2] + processor->imgdata.color.cblack[3]) / 4;
    } else {
        black_level = processor->imgdata.color.black;
    }
    white_level = processor->imgdata.color.maximum;

    std::string pattern_str(processor->imgdata.idata.cdesc);
    if (pattern_str == "GRBG") cfa_pattern = 0;
    else if (pattern_str == "RGGB") cfa_pattern = 1;
    else if (pattern_str == "GBRG") cfa_pattern = 2;
    else if (pattern_str == "BGGR") cfa_pattern = 3;
    else { cfa_pattern = 0; }

    uint16_t* data = (uint16_t*)((uint8_t*)processor->imgdata.rawdata.raw_image +
                                 (int64_t)processor->imgdata.sizes.top_margin * pitch) +
                                 processor->imgdata.sizes.left_margin;
    int stride_elements = pitch / sizeof(uint16_t);

    std::vector<halide_dimension_t> shape = {{0, width, 1}, {0, height, stride_elements}};
    Halide::Runtime::Buffer<uint16_t, 2> halide_buffer(data, shape);

    return {halide_buffer, processor};
}


int main(int argc, char** argv) {
    AppState app_state;
    try {
        app_state.params = parse_args(argc, argv);
    } catch (const std::runtime_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        print_usage();
        return 1;
    }
    
    if (app_state.params.input_path.empty()) {
        std::cout << "Usage: ./rawr <path_to_image.png> [options...]" << std::endl;
        return 1;
    }

    // Initialize curve points from config, or create a default linear curve if none were provided.
    auto ensure_default_curve = [](std::vector<Point>& points){
        if (points.empty()) {
            points.push_back({0.0f, 0.0f});
            points.push_back({1.0f, 1.0f});
        }
    };
    ensure_default_curve(app_state.params.curve_points_luma);
    ensure_default_curve(app_state.params.curve_points_r);
    ensure_default_curve(app_state.params.curve_points_g);
    ensure_default_curve(app_state.params.curve_points_b);

    // Eagerly allocate the tone curve LUT buffers to prevent crashes.
    // They have a fixed size, so we can do this once at startup.
    app_state.pipeline_tone_curve_lut = Halide::Runtime::Buffer<uint16_t, 2>(65536, 3);
    app_state.ui_tone_curve_lut = Halide::Runtime::Buffer<uint16_t, 2>(65536, 3);

    // --- Initialize Lensfun Database ---
#ifdef USE_LENSFUN
    app_state.lensfun_db.reset(new lfDatabase());
    if (app_state.lensfun_db) {
        app_state.lensfun_db->Load();
        const lfCamera *const *cameras = app_state.lensfun_db->GetCameras();
        std::set<std::string> makes;
        if (cameras) {
            for (int i = 0; cameras[i]; i++) {
                makes.insert(cameras[i]->Maker);
            }
        }
        app_state.lensfun_camera_makes.assign(makes.begin(), makes.end());
        std::sort(app_state.lensfun_camera_makes.begin(), app_state.lensfun_camera_makes.end());
    } else {
        std::cerr << "Warning: Could not create Lensfun database." << std::endl;
    }
#endif

    // --- Load Input Image ---
    try {
        if (app_state.params.raw_png) {
             app_state.input_image = Halide::Tools::load_and_convert_image(app_state.params.input_path);
        } else {
            auto result = load_raw_file_nocopy_ui(app_state.params.input_path,
                                                  app_state.cfa_pattern,
                                                  app_state.blackLevel,
                                                  app_state.whiteLevel);
            app_state.input_image = std::get<0>(result);
            app_state.raw_processor = std::get<1>(result);
        }
        std::cout << "Loaded image: " << app_state.params.input_path << " (" 
                  << app_state.input_image.width() << "x" << app_state.input_image.height() << ")" << std::endl;
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to load input image: " << e.what() << std::endl;
        return 1;
    }
    
    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("Rawr Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Load the custom font from a file relative to the executable.
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        std::string font_path = std::string(base_path) + "Inter-Regular.ttf";
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f);
        SDL_free(base_path);
    } else {
        io.Fonts->AddFontFromFileTTF("Inter-Regular.ttf", 16.0f);
    }

    // Apply the custom theme
    SetRawrTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Main event-driven loop
    bool done = false;

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT || 
               (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)))
            {
                done = true;
            }
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        
        // Render all UI components and handle pipeline execution internally
        RenderUI(app_state);

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
        }

        SDL_GL_SwapWindow(window);
        SDL_Delay(1); 
    }

    // Cleanup
    DeleteTexture(app_state.main_texture_id);
    DeleteTexture(app_state.thumb_texture_id);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

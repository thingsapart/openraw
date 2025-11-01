#include <SDL.h>
#include <iostream>
#include <vector>

// Use GLES 3
#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl3.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "IconsFontAwesome6.h"

#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

// --- GLSL Shaders for the camera view ---
const char* vertexShaderSource = R"(
    #version 300 es
    layout (location = 0) in vec3 aPos;
    layout (location = 1) in vec2 aTexCoord;
    out vec2 TexCoord;
    void main()
    {
        gl_Position = vec4(aPos, 1.0);
        TexCoord = aTexCoord;
    }
)";

const char* fragmentShaderSource = R"(
    #version 300 es
    precision mediump float;
    out vec4 FragColor;
    in vec2 TexCoord;
    uniform sampler2D ourTexture;
    void main()
    {
        FragColor = texture(ourTexture, TexCoord);
    }
)";

// --- OpenGL Helper Functions ---
GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::COMPILATION_FAILED\n" << infoLog << std::endl;
    }
    return shader;
}

GLuint CreateShaderProgram(const char* vsSource, const char* fsSource) {
    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vsSource);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "ERROR::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl;
    }
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return program;
}


// --- Application State ---
struct CameraState {
    cv::VideoCapture cap;
    cv::Mat frame;
    int width = 1280;
    int height = 720;
};

struct RendererState {
    GLuint shaderProgram;
    GLuint textureID;
    GLuint vao;
};

// --- Per-Display Resources ---
struct Display {
    int display_index;
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    ImGuiContext* imgui_context = nullptr;
    int width = 0;
    int height = 0;
};

// --- Declarative UI Structures ---
// Forward declaration for nested menus
struct MenuItem;
using MenuPage = std::vector<MenuItem>;

// We can add more types like Toggle, Enum, Slider later
enum class MenuItemType {
    Submenu,    // Navigates to another page of items
    StaticText  // A simple non-interactive text label
};

struct MenuItem {
    std::string label;
    MenuItemType type;
    MenuPage submenu_page; // Only used by Submenu type
};

struct SettingsTab {
    std::string name;
    MenuPage root_page;
};

struct AppState {
    bool show_settings_window = false;

    // UI Navigation State
    size_t active_tab_idx = 0;
    // A stack representing the path of submenus taken. The back() is the current page.
    std::vector<MenuPage*> navigation_stack;

    // This will hold our entire menu definition
    std::vector<SettingsTab> settings_tabs;

    // Placeholder for actual calibration values
    float k1 = 0.0f, k2 = 0.0f, p1 = 0.0f, p2 = 0.0f;
    float fx = 1280.0f, fy = 720.0f, cx = 640.0f, cy = 360.0f;
};


// --- UI Logic ---

void InitializeSettings(AppState& state) {
    state.settings_tabs = {
        {
            "Calibration", // Tab Name
            { // Root Page
                { 
                    "Calibrate", 
                    MenuItemType::Submenu,
                    { // Sub-page for "Calibrate"
                        { "This is where calibration sliders will go.", MenuItemType::StaticText, {} },
                    }
                },
            }
        },
        {
            "About", // Tab Name
            { // Root Page
                { "PiCam Frontend", MenuItemType::StaticText, {} },
                { "Version 0.1.0", MenuItemType::StaticText, {} },
            }
        }
    };
}

// Renders a single page of menu items
void RenderMenuPage(AppState& state, MenuPage& page) {
    // Back button for submenus
    if (state.navigation_stack.size() > 1) {
        if (ImGui::Button(ICON_FA_ARROW_LEFT " Back")) {
            state.navigation_stack.pop_back();
        }
        ImGui::Separator();
    }

    // Render each item on the page
    for (auto& item : page) {
        switch (item.type) {
            case MenuItemType::Submenu:
                ImGui::PushID(&item); // Ensure unique ID for items with same label
                if (ImGui::Selectable(item.label.c_str(), false, 0, ImVec2(0, ImGui::GetTextLineHeightWithSpacing()))){
                    state.navigation_stack.push_back(&item.submenu_page);
                }
                ImGui::SameLine(ImGui::GetWindowWidth() - 30);
                ImGui::Text(ICON_FA_CHEVRON_RIGHT);
                ImGui::PopID();
                break;

            case MenuItemType::StaticText:
                ImGui::TextWrapped("%s", item.label.c_str());
                break;
        }
    }
}

// Main UI rendering function
void RenderUI(Display& /*display*/, AppState& state) {
    // This function now operates on the currently active ImGui context
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display_size = io.DisplaySize;

    // --- Top and Bottom Bars (unchanged) ---
    const float top_bar_height = 40.0f;
    const float bottom_bar_height = 60.0f;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(display_size.x, top_bar_height));
    ImGui::Begin("TopBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(display_size.x, top_bar_height), IM_COL32(0, 0, 0, 180));
    ImGui::SetCursorPos(ImVec2(10, 8));
    ImGui::Text("PiCam Live View");
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(0, display_size.y - bottom_bar_height));
    ImGui::SetNextWindowSize(ImVec2(display_size.x, bottom_bar_height));
    ImGui::Begin("BottomBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const char* settings_text = ICON_FA_GEAR " Settings";
    ImVec2 settings_text_size = ImGui::CalcTextSize(settings_text);
    ImGui::SetCursorPosX((display_size.x - settings_text_size.x) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bottom_bar_height - settings_text_size.y) * 0.5f - ImGui::GetStyle().FramePadding.y);
    if (ImGui::Button(settings_text, ImVec2(settings_text_size.x + 40, settings_text_size.y + 10))) {
        state.show_settings_window = !state.show_settings_window;
        // When opening the window, reset navigation to the root of the active tab
        if (state.show_settings_window) {
            state.navigation_stack.clear();
        }
    }
    ImGui::End();


    // --- Declarative Settings Window ---
    if (state.show_settings_window) {
        ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings", &state.show_settings_window);

        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < state.settings_tabs.size(); ++i) {
                auto& tab = state.settings_tabs[i];
                if (ImGui::BeginTabItem(tab.name.c_str())) {
                    // If tab changes, reset navigation
                    if (state.active_tab_idx != i) {
                        state.active_tab_idx = i;
                        state.navigation_stack.clear();
                    }
                    
                    // Ensure navigation stack is initialized
                    if (state.navigation_stack.empty()) {
                        state.navigation_stack.push_back(&tab.root_page);
                    }

                    RenderMenuPage(state, *state.navigation_stack.back());
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
}


int main(int, char**) {
    // --- Setup SDL ---
    // Enable verbose logging to help debug display initialization issues.
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);

    // Hint SDL to use the KMS/DRM video driver. This is crucial for detecting
    // multiple displays (like DSI and HDMI) on embedded systems without a
    // full X11 desktop environment.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // --- Setup Window with OpenGL ES 3 context ---
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // --- Create a window and context for each display ---
    std::vector<Display> displays;
    IMGUI_CHECKVERSION();

    int num_displays = SDL_GetNumVideoDisplays();
    if (num_displays < 1) {
        std::cerr << "Error: No displays found by SDL." << std::endl;
        return -1;
    }

    std::cout << "Found " << num_displays << " displays. Initializing..." << std::endl;

    for (int i = 0; i < num_displays; ++i) {
        Display d;
        d.display_index = i;

        SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
        d.window = SDL_CreateWindow(
            "PiCam Frontend",
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(i),
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(i),
            0, 0, // Ignored for FULLSCREEN_DESKTOP
            window_flags
        );

        if (d.window == nullptr) {
            std::cerr << "Error: SDL_CreateWindow() for display " << i << ": " << SDL_GetError() << std::endl;
            continue;
        }
        
        SDL_GetWindowSize(d.window, &d.width, &d.height);
        std::cout << "  - Display " << i << ": Window created with size " << d.width << "x" << d.height << std::endl;

        d.gl_context = SDL_GL_CreateContext(d.window);
        if (d.gl_context == nullptr) {
            std::cerr << "Error: SDL_GL_CreateContext() for display " << i << ": " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(d.window);
            continue;
        }

        SDL_GL_MakeCurrent(d.window, d.gl_context);
        SDL_GL_SetSwapInterval(1); // Enable vsync

        // Setup Dear ImGui context for this display
        d.imgui_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(d.imgui_context);

        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.IniFilename = nullptr; // Disable imgui.ini saving
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        ImGui_ImplSDL2_InitForOpenGL(d.window, d.gl_context);
        ImGui_ImplOpenGL3_Init("#version 300 es");

        // Load fonts for this context
        io.Fonts->AddFontFromFileTTF("assets/Roboto-Regular.ttf", 20.0f);
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        config.GlyphMinAdvanceX = 20.0f;
        static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
        io.Fonts->AddFontFromFileTTF("assets/FontAwesome6-Solid-900.otf", 16.0f, &config, icon_ranges);

        displays.push_back(d);
    }

    if (displays.empty()) {
        std::cerr << "Error: Failed to create a window on any display." << std::endl;
        SDL_Quit();
        return -1;
    }

    // --- Initialize Camera (Shared Resource) ---
    CameraState camera;
    camera.cap.open(0, cv::CAP_V4L2); // Use V4L2 backend
    if (!camera.cap.isOpened()) {
        std::cerr << "Error: Could not open webcam." << std::endl;
        return -1;
    }
    camera.cap.set(cv::CAP_PROP_FRAME_WIDTH, camera.width);
    camera.cap.set(cv::CAP_PROP_FRAME_HEIGHT, camera.height);

    // --- Setup Renderer for Camera View (Shared Resources) ---
    RendererState renderer;
    // Set GL context for shared resource creation
    SDL_GL_MakeCurrent(displays[0].window, displays[0].gl_context);
    renderer.shaderProgram = CreateShaderProgram(vertexShaderSource, fragmentShaderSource);
    glGenTextures(1, &renderer.textureID);
    glBindTexture(GL_TEXTURE_2D, renderer.textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, camera.width, camera.height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    float vertices[] = {
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f
    };
    unsigned int indices[] = { 0, 1, 3, 1, 2, 3 };
    GLuint VBO, EBO;
    glGenVertexArrays(1, &renderer.vao);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(renderer.vao);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // --- Initialize Application State ---
    AppState state;
    InitializeSettings(state);
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);

    // --- Main loop ---
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Find the display associated with the event's window ID
            Display* event_display = nullptr;
            if (event.type == SDL_WINDOWEVENT) {
                for (auto& d : displays) {
                    if (event.window.windowID == SDL_GetWindowID(d.window)) {
                        event_display = &d;
                        break;
                    }
                }
            }
            // For other events, or if no specific display found, default to the first one
            if (!event_display && !displays.empty()) {
                event_display = &displays[0];
            }

            if (event_display) {
                ImGui::SetCurrentContext(event_display->imgui_context);
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
                done = true;
            }
        }

        // --- Get ONE Camera Frame for this iteration ---
        camera.cap >> camera.frame;
        if (!camera.frame.empty()) {
            cv::cvtColor(camera.frame, camera.frame, cv::COLOR_BGR2RGB);
            glBindTexture(GL_TEXTURE_2D, renderer.textureID);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, camera.width, camera.height, GL_RGB, GL_UNSIGNED_BYTE, camera.frame.data);
        }

        // --- Render to each display ---
        for (auto& display : displays) {
            SDL_GL_MakeCurrent(display.window, display.gl_context);
            ImGui::SetCurrentContext(display.imgui_context);

            glViewport(0, 0, display.width, display.height);
            glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
            glClear(GL_COLOR_BUFFER_BIT);

            // 1. Render Camera View (background)
            glUseProgram(renderer.shaderProgram);
            glBindVertexArray(renderer.vao);
            glBindTexture(GL_TEXTURE_2D, renderer.textureID);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            
            // 2. Render ImGui UI (foreground)
            RenderUI(display, state);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        // Swap all windows after rendering is complete for all of them
        for (auto& display : displays) {
            SDL_GL_SwapWindow(display.window);
        }
    }

    // --- Cleanup ---
    camera.cap.release();
    glDeleteVertexArrays(1, &renderer.vao);
    glDeleteTextures(1, &renderer.textureID);
    glDeleteProgram(renderer.shaderProgram);

    for (auto& display : displays) {
        ImGui::SetCurrentContext(display.imgui_context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(display.imgui_context);
        SDL_GL_DeleteContext(display.gl_context);
        SDL_DestroyWindow(display.window);
    }
    
    SDL_Quit();

    return 0;
}

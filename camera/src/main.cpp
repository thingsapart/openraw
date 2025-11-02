#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <csignal>

// DRM/KMS, GBM, and EGL headers
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

// Use GLES 3
#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl3.h>

#include "imgui.h"
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
    int rotation_angle = -1; // OpenCV rotation code, e.g., cv::ROTATE_180. -1 means no rotation.
};

struct RendererState {
    GLuint shaderProgram;
    GLuint textureID;
    GLuint vao;
};

// --- DRM/GBM/EGL Backend Structures ---

struct DrmBackend; // Forward declaration

// All resources related to a single connected display.
struct Display {
    DrmBackend* backend = nullptr;
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    drmModeModeInfo mode = {};
    drmModeCrtc* saved_crtc = nullptr;

    struct gbm_surface* gbm_surf = nullptr;
    EGLContext egl_ctx = EGL_NO_CONTEXT;
    EGLSurface egl_surf = EGL_NO_SURFACE;

    ImGuiContext* imgui_context = nullptr;

    struct gbm_bo* current_bo = nullptr;
    uint32_t current_fb_id = 0;

    bool page_flip_pending = false;

    // Keep width/height for convenience, populated from 'mode'
    int width = 0;
    int height = 0;
};

// Resources shared across all displays on a single GPU.
struct DrmBackend {
    std::string device_path;
    int fd = -1;
    struct gbm_device* gbm_dev = nullptr;
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    EGLConfig egl_conf = nullptr;
    EGLContext shared_egl_ctx = EGL_NO_CONTEXT;
    RendererState renderer;
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
void RenderUI(AppState& state) {
    // This function now operates on the currently active ImGui context
    ImGui_ImplOpenGL3_NewFrame();
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

    // Button is disabled as we have no input handling yet.
    ImGui::BeginDisabled();
    if (ImGui::Button(settings_text, ImVec2(settings_text_size.x + 40, settings_text_size.y + 10))) {
        state.show_settings_window = !state.show_settings_window;
        if (state.show_settings_window) {
            state.navigation_stack.clear();
        }
    }
    ImGui::EndDisabled();
    ImGui::End();


    // --- Declarative Settings Window ---
    if (state.show_settings_window) {
        ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings", &state.show_settings_window);

        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < state.settings_tabs.size(); ++i) {
                auto& tab = state.settings_tabs[i];
                if (ImGui::BeginTabItem(tab.name.c_str())) {
                    if (state.active_tab_idx != i) {
                        state.active_tab_idx = i;
                        state.navigation_stack.clear();
                    }
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

// --- Renderer Setup ---
void InitializeRenderer(RendererState& renderer, int camera_width, int camera_height) {
    renderer.shaderProgram = CreateShaderProgram(vertexShaderSource, fragmentShaderSource);
    glGenTextures(1, &renderer.textureID);
    glBindTexture(GL_TEXTURE_2D, renderer.textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, camera_width, camera_height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    float vertices[] = {
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f,  1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, -1.0f,  1.0f, 0.0f,   0.0f, 0.0f
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
}


// --- DRM/GBM/EGL Helper Functions ---

// Callback for page flip events.

// Callback for page flip events.
void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data) {
    Display* display = static_cast<Display*>(data);
    display->page_flip_pending = false;
}

// Helper to get a DRM framebuffer ID for a GBM buffer object.
// Caches the ID in the BO's user data.
uint32_t get_fb_for_bo(int drm_fd, struct gbm_bo* bo) {
    uint32_t* p_fb_id = static_cast<uint32_t*>(gbm_bo_get_user_data(bo));
    if (p_fb_id && *p_fb_id != 0) {
        return *p_fb_id;
    }

    uint32_t fb_id;
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t format = gbm_bo_get_format(bo);
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};

    int num_planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < num_planes; ++i) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = gbm_bo_get_modifier(bo);
    }

    if (drmModeAddFB2WithModifiers(drm_fd, width, height, format, handles, strides, offsets, modifiers, &fb_id, DRM_MODE_FB_MODIFIERS) != 0) {
        std::cerr << "Failed to create DRM framebuffer" << std::endl;
        return 0;
    }

    // Store the fb_id in the user data of the bo for reuse.
    uint32_t* new_fb_id = new uint32_t(fb_id);
    gbm_bo_set_user_data(bo, new_fb_id, [](struct gbm_bo*, void* data){
        delete static_cast<uint32_t*>(data);
    });

    return fb_id;
}


// Global flag to signal main loop to exit
volatile bool done = false;

void signal_handler(int) {
    done = true;
}

int main(int, char**) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // --- Initialize Camera (Shared Resource) ---
    // We do this *before* initializing DRM/display devices to avoid potential
    // resource conflicts where initializing a non-display DRM device (like a
    // camera controller) might interfere with the V4L2 subsystem.
    CameraState camera;
    camera.rotation_angle = cv::ROTATE_180; // Correct for upside-down camera mounting

    // Attempt to open the camera with default settings.
    // Using CAP_V4L2 is good practice on Linux.
    camera.cap.open(0, cv::CAP_V4L2);
    if (!camera.cap.isOpened()) {
        std::cerr << "Error: Could not open webcam." << std::endl;
        return -1;
    }

    // Set desired resolution. The camera will use the closest supported resolution.
    camera.cap.set(cv::CAP_PROP_FRAME_WIDTH, camera.width);
    camera.cap.set(cv::CAP_PROP_FRAME_HEIGHT, camera.height);

    // Let the camera use its default/auto settings for exposure and frame rate.
    // This is the most compatible approach.
    std::cout << "Camera opened successfully." << std::endl;
    double actual_fps = camera.cap.get(cv::CAP_PROP_FPS);
    if (actual_fps > 0) {
        std::cout << "Camera default FPS: " << actual_fps << std::endl;
    }

    // --- Setup DRM/GBM/EGL for all available cards ---
    std::vector<std::unique_ptr<DrmBackend>> backends;
    std::vector<std::unique_ptr<Display>> displays;

    // --- Find and initialize all available DRM devices ---
    for (int i = 0; i < 4; ++i) { // Simple scan for card0 to card3
        std::string card_path = "/dev/dri/card" + std::to_string(i);
        int fd = open(card_path.c_str(), O_RDWR);
        if (fd < 0) continue;

        // Immediately check if this DRM device has any display capability.
        // This prevents us from grabbing non-display devices (like camera
        // receivers) which can cause conflicts with V4L2.
        drmModeRes* res = drmModeGetResources(fd);
        if (!res || res->count_connectors == 0) {
            drmModeFreeResources(res);
            close(fd);
            continue;
        }
        drmModeFreeResources(res); // We only needed to check for existence.

        auto backend = std::make_unique<DrmBackend>();
        backend->device_path = card_path;
        backend->fd = fd;

        backend->gbm_dev = gbm_create_device(backend->fd);
        if (!backend->gbm_dev) {
            std::cerr << "Error: Could not create GBM device for " << card_path << std::endl;
            close(backend->fd);
            continue;
        }

        backend->egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, backend->gbm_dev, NULL);
        if (backend->egl_dpy == EGL_NO_DISPLAY) {
            std::cerr << "Error: Could not get EGL display for " << card_path << std::endl;
            gbm_device_destroy(backend->gbm_dev);
            close(backend->fd);
            continue;
        }

        EGLint major, minor;
        if (!eglInitialize(backend->egl_dpy, &major, &minor)) {
            std::cerr << "Error: Could not initialize EGL for " << card_path << std::endl;
            gbm_device_destroy(backend->gbm_dev);
            close(backend->fd);
            continue;
        }
        eglBindAPI(EGL_OPENGL_ES_API);

        const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
        };
        EGLint num_config;
        if (!eglChooseConfig(backend->egl_dpy, config_attribs, &backend->egl_conf, 1, &num_config) || num_config != 1) {
            std::cerr << "Error: Could not choose EGL config for " << card_path << std::endl;
            eglTerminate(backend->egl_dpy);
            gbm_device_destroy(backend->gbm_dev);
            close(backend->fd);
            continue;
        }

        const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        backend->shared_egl_ctx = eglCreateContext(backend->egl_dpy, backend->egl_conf, EGL_NO_CONTEXT, context_attribs);
        if (backend->shared_egl_ctx == EGL_NO_CONTEXT) {
            std::cerr << "Error: Could not create shared EGL context for " << card_path << std::endl;
            eglTerminate(backend->egl_dpy);
            gbm_device_destroy(backend->gbm_dev);
            close(backend->fd);
            continue;
        }

        std::cout << "Initialized backend for " << card_path << std::endl;
        backends.push_back(std::move(backend));
    }

    if (backends.empty()) {
        std::cerr << "Error: Could not initialize any DRM devices." << std::endl;
        return -1;
    }

    // --- Find and set up displays on all initialized backends ---
    for (const auto& backend_ptr : backends) {
        DrmBackend* backend = backend_ptr.get();
        drmModeRes* resources = drmModeGetResources(backend->fd);
        if (!resources) {
            // This is expected for devices like camera receivers that don't have display outputs.
            std::cout << "Info: DRM device " << backend->device_path << " has no display resources, skipping." << std::endl;
            continue;
        }

        for (int i = 0; i < resources->count_connectors; ++i) {
            drmModeConnector* connector = drmModeGetConnector(backend->fd, resources->connectors[i]);
            if (!connector || connector->connection != DRM_MODE_CONNECTED || connector->count_modes == 0) {
                drmModeFreeConnector(connector);
                continue;
            }

            auto display = std::make_unique<Display>();
            display->backend = backend;
            display->connector_id = connector->connector_id;
            display->mode = connector->modes[0];
            display->width = display->mode.hdisplay;
            display->height = display->mode.vdisplay;

            drmModeEncoder* encoder = drmModeGetEncoder(backend->fd, connector->encoder_id);
            if (!encoder) {
                std::cerr << "Could not get encoder for connector " << i << std::endl;
                drmModeFreeConnector(connector);
                continue;
            }
            display->crtc_id = encoder->crtc_id;
            display->saved_crtc = drmModeGetCrtc(backend->fd, display->crtc_id);
            drmModeFreeEncoder(encoder);

            // Per-display EGL/GBM/ImGui setup
            display->gbm_surf = gbm_surface_create(backend->gbm_dev, display->width, display->height,
                GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
            if (!display->gbm_surf) {
                std::cerr << "Error: Could not create GBM surface for connector " << display->connector_id << std::endl;
                drmModeFreeCrtc(display->saved_crtc);
                drmModeFreeConnector(connector);
                continue;
            }

            display->egl_surf = eglCreateWindowSurface(backend->egl_dpy, backend->egl_conf, (EGLNativeWindowType)display->gbm_surf, NULL);
            if (display->egl_surf == EGL_NO_SURFACE) {
                std::cerr << "Error: Could not create EGL window surface (EGL error: " << std::hex << eglGetError() << ")" << std::endl;
                gbm_surface_destroy(display->gbm_surf);
                drmModeFreeCrtc(display->saved_crtc);
                drmModeFreeConnector(connector);
                continue;
            }

            const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            display->egl_ctx = eglCreateContext(backend->egl_dpy, backend->egl_conf, backend->shared_egl_ctx, context_attribs);
            if (display->egl_ctx == EGL_NO_CONTEXT) {
                std::cerr << "Error: Could not create EGL context (EGL error: " << std::hex << eglGetError() << ")" << std::endl;
                eglDestroySurface(backend->egl_dpy, display->egl_surf);
                gbm_surface_destroy(display->gbm_surf);
                drmModeFreeCrtc(display->saved_crtc);
                drmModeFreeConnector(connector);
                continue;
            }


            if (eglMakeCurrent(backend->egl_dpy, display->egl_surf, display->egl_surf, display->egl_ctx) == EGL_FALSE) {
                std::cerr << "Error: Could not make EGL context current (EGL error: " << std::hex << eglGetError() << ")" << std::endl;
                eglDestroyContext(backend->egl_dpy, display->egl_ctx);
                eglDestroySurface(backend->egl_dpy, display->egl_surf);
                gbm_surface_destroy(display->gbm_surf);
                drmModeFreeCrtc(display->saved_crtc);
                drmModeFreeConnector(connector);
                continue;
            }

            IMGUI_CHECKVERSION();
            display->imgui_context = ImGui::CreateContext();
            ImGui::SetCurrentContext(display->imgui_context);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)display->width, (float)display->height);
            io.IniFilename = nullptr;

            ImGui::StyleColorsDark();
            ImGui_ImplOpenGL3_Init("#version 300 es");

            io.Fonts->AddFontFromFileTTF("assets/Roboto-Regular.ttf", 20.0f);
            ImFontConfig config;
            config.MergeMode = true;
            config.PixelSnapH = true;
            config.GlyphMinAdvanceX = 20.0f;
            static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
            io.Fonts->AddFontFromFileTTF("assets/FontAwesome6-Solid-900.otf", 16.0f, &config, icon_ranges);

            std::cout << "  - Found display on connector " << display->connector_id << " (" << display->width << "x" << display->height << ")" << std::endl;
            displays.push_back(std::move(display));
            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
    }

    if (displays.empty()) {
        std::cerr << "Error: No connected displays found across all backends." << std::endl;
        // Cleanup logic will run at the end of main
        return -1;
    }

    // --- Setup a renderer for each backend ---
    for (const auto& backend : backends) {
        // Find a display that belongs to this backend to get a valid EGL context
        Display* display_for_context = nullptr;
        for (const auto& d : displays) {
            if (d->backend == backend.get()) {
                display_for_context = d.get();
                break;
            }
        }
        // If a backend has no displays, we don't need to create renderer resources for it
        if (display_for_context) {
            eglMakeCurrent(backend->egl_dpy, display_for_context->egl_surf, display_for_context->egl_surf, display_for_context->egl_ctx);
            InitializeRenderer(backend->renderer, camera.width, camera.height);
        }
    }


    // --- Initial display setup (modesetting) ---
    for(const auto& display : displays) {
        // Set the correct context before performing operations on its surface.
        eglMakeCurrent(display->backend->egl_dpy, display->egl_surf, display->egl_surf, display->egl_ctx);
        eglSwapBuffers(display->backend->egl_dpy, display->egl_surf);
        struct gbm_bo* bo = gbm_surface_lock_front_buffer(display->gbm_surf);
        uint32_t fb_id = get_fb_for_bo(display->backend->fd, bo);
        drmModeSetCrtc(display->backend->fd, display->crtc_id, fb_id, 0, 0, &display->connector_id, 1, &display->mode);
        display->current_bo = bo;
        display->current_fb_id = fb_id;
    }

    // --- Initialize Application State ---
    AppState state;
    InitializeSettings(state);
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    auto last_time = std::chrono::high_resolution_clock::now();

    // --- Main loop ---
    while (!done) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float delta_time = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;

        camera.cap >> camera.frame;
        if (!camera.frame.empty()) {
            // Apply rotation if specified
            if (camera.rotation_angle >= 0) {
                cv::rotate(camera.frame, camera.frame, camera.rotation_angle);
            }
            cv::cvtColor(camera.frame, camera.frame, cv::COLOR_BGR2RGB);

            // Upload the frame to each backend's texture
            for (const auto& backend : backends) {
                if (backend->renderer.textureID == 0) continue; // Skip backends with no displays/renderer

                // We need a context to be current to do GL operations. We can just use the
                // first display associated with this backend.
                Display* display_for_context = nullptr;
                for (const auto& d : displays) {
                    if (d->backend == backend.get()) {
                        display_for_context = d.get();
                        break;
                    }
                }
                if (display_for_context) {
                    eglMakeCurrent(backend->egl_dpy, display_for_context->egl_surf, display_for_context->egl_surf, display_for_context->egl_ctx);
                    glBindTexture(GL_TEXTURE_2D, backend->renderer.textureID);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, camera.width, camera.height, GL_RGB, GL_UNSIGNED_BYTE, camera.frame.data);
                }
            }
        }

        for (const auto& display : displays) {
            if (display->page_flip_pending) continue;

            eglMakeCurrent(display->backend->egl_dpy, display->egl_surf, display->egl_surf, display->egl_ctx);
            ImGui::SetCurrentContext(display->imgui_context);
            ImGui::GetIO().DeltaTime = delta_time > 0.0f ? delta_time : 1.0f/60.0f;

            glViewport(0, 0, display->width, display->height);
            glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            glClear(GL_COLOR_BUFFER_BIT);

            // Use the renderer associated with this display's backend
            RendererState& renderer = display->backend->renderer;
            glUseProgram(renderer.shaderProgram);
            glBindVertexArray(renderer.vao);
            glBindTexture(GL_TEXTURE_2D, renderer.textureID);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

            RenderUI(state);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            eglSwapBuffers(display->backend->egl_dpy, display->egl_surf);
            struct gbm_bo* next_bo = gbm_surface_lock_front_buffer(display->gbm_surf);
            uint32_t next_fb_id = get_fb_for_bo(display->backend->fd, next_bo);

            if (drmModePageFlip(display->backend->fd, display->crtc_id, next_fb_id, DRM_MODE_PAGE_FLIP_EVENT, display.get()) == 0) {
                display->page_flip_pending = true;
                gbm_surface_release_buffer(display->gbm_surf, display->current_bo);
                display->current_bo = next_bo;
                display->current_fb_id = next_fb_id;
            } else {
                 std::cerr << "drmModePageFlip failed" << std::endl;
            }
        }

        drmEventContext ev_ctx = {};
        ev_ctx.version = 2;
        ev_ctx.page_flip_handler = page_flip_handler;
        fd_set fds;
        FD_ZERO(&fds);
        int max_fd = -1;
        for(const auto& backend : backends) {
            FD_SET(backend->fd, &fds);
            if (backend->fd > max_fd) max_fd = backend->fd;
        }

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 500000 };
        int ret = select(max_fd + 1, &fds, NULL, NULL, &timeout);
        if (ret > 0) {
            for(const auto& backend : backends) {
                if (FD_ISSET(backend->fd, &fds)) {
                    drmHandleEvent(backend->fd, &ev_ctx);
                }
            }
        }
    }

    // --- Cleanup ---
    std::cout << "Cleaning up..." << std::endl;
    camera.cap.release();
    // Clean up GL resources for each backend
    for (const auto& backend : backends) {
        if (backend->renderer.shaderProgram == 0) continue; // Skip backends with no renderer

        Display* display_for_context = nullptr;
        for (const auto& d : displays) {
            if (d->backend == backend.get()) {
                display_for_context = d.get();
                break;
            }
        }
        if (display_for_context) {
            eglMakeCurrent(backend->egl_dpy, display_for_context->egl_surf, display_for_context->egl_surf, display_for_context->egl_ctx);
            glDeleteVertexArrays(1, &backend->renderer.vao);
            glDeleteTextures(1, &backend->renderer.textureID);
            glDeleteProgram(backend->renderer.shaderProgram);
        }
    }


    for (const auto& display : displays) {
        ImGui::SetCurrentContext(display->imgui_context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(display->imgui_context);

        drmModeSetCrtc(display->backend->fd, display->saved_crtc->crtc_id, display->saved_crtc->buffer_id,
            display->saved_crtc->x, display->saved_crtc->y, &display->connector_id, 1, &display->saved_crtc->mode);
        drmModeFreeCrtc(display->saved_crtc);
        if (display->current_bo) gbm_surface_release_buffer(display->gbm_surf, display->current_bo);
        eglDestroySurface(display->backend->egl_dpy, display->egl_surf);
        gbm_surface_destroy(display->gbm_surf);
        eglDestroyContext(display->backend->egl_dpy, display->egl_ctx);
    }

    for (const auto& backend : backends) {
        if (backend->shared_egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(backend->egl_dpy, backend->shared_egl_ctx);
        if (backend->egl_dpy != EGL_NO_DISPLAY) eglTerminate(backend->egl_dpy);
        if (backend->gbm_dev) gbm_device_destroy(backend->gbm_dev);
        if (backend->fd >= 0) close(backend->fd);
    }

    return 0;
}

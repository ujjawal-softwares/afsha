#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <stdio.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <thread>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "whisper.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "src/app_config.h"
#include "wav_writer.h"
#define WHISPER_SAMPLE_RATE 16000

// We are using ImGUI for creating any UI elements.
// Dear ImGui: standalone example application for SDL3 + OpenGL
// (SDL is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp


static const int WIDTH = 800;
static const int HEIGHT = 480;
static const int PADDING = 10;
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_AudioStream *stream = NULL;
static ImGuiIO *ioRef = NULL;
static ImFont *font = NULL;

static bool show_demo_window = true;

const std::string APP_VERSION = APP_VERSION_MAJOR + \
        std::string(".") + \
        APP_VERSION_MINOR + \
        std::string(".") + \
        APP_VERSION_PATCH;

static const std::string WINDOW_TITLE = APP_NAME + std::string(" ") + APP_VERSION;

static const int n_samples_step = (1e-3 * 3000) * WHISPER_SAMPLE_RATE;
static const int n_samples_len  = (1e-3 * 10000) * WHISPER_SAMPLE_RATE;

// TODO: We sometimes need to pla around with n_samples_keep to get the best results
static const int n_samples_keep = (1e-3 * 100) * WHISPER_SAMPLE_RATE;
static const int n_samples_30s  = (1e-3 * 30000) * WHISPER_SAMPLE_RATE;
static const int AUDIO_CHUNK_SIZE = 10240;
static const int AUDIO_MAX_CHUNK_SIZE = AUDIO_CHUNK_SIZE * 10;
// overallocate the audio buffer to avoid reallocation
static const std::vector<float> audio_buffer(AUDIO_MAX_CHUNK_SIZE, 0.0f);
static int audio_buffer_pos = 0;

// We will keep filling this buffer with new audio samples in audio_buffer
// once the size reaches n_samples_step, we will try to do speech recognition
static std::vector<float> audio_buffer_for_speech_recognition(n_samples_30s, 0.0f);
static int audio_buffer_for_speech_recognition_pos = 0;
static std::mutex audio_buffer_for_speech_recognition_mutex;

// why do we need old audio buffer? because we need n_samples_keep samples from the old buffer
// so that we can concatenate it with the new buffer
static std::vector<float> audio_buffer_for_speech_recognition_old(n_samples_keep, 0.0f);
static int audio_buffer_for_speech_recognition_old_pos = 0;

static std::vector<float> audio_buffer_for_speech_recognition_combined(n_samples_30s + n_samples_keep, 0.0f);

// They are used to store the tokens from the last full length segment as the prompt
static std::vector<whisper_token> prompt_tokens_for_speech_recognition(n_samples_30s);


static struct whisper_context *whisper_ctx = NULL;
// static std::string audio_filename;
// static wav_writer wavWriter;

static std::deque<std::string> text_speech_recognition;
static int text_speech_recognition_size = 1000;

static SDL_CameraID camera_id = 0;
static SDL_Camera *camera = NULL;
static SDL_CameraSpec spec;

static SDL_Surface *current_frame = NULL;
static SDL_Texture *current_frame_texture = NULL;
static bool current_frame_texture_updated = false;
static int video_stream_width = 0;
static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);


// TODO: We can parse this from the command line arguments
// struct whisper_params {
//     int32_t n_threads  = std::min(4, (int32_t) std::thread::hardware_concurrency());
//     int32_t step_ms    = 3000;
//     int32_t length_ms  = 10000;
//     int32_t keep_ms    = 200;
//     int32_t capture_id = -1;
//     int32_t max_tokens = 32;
//     int32_t audio_ctx  = 0;

//     float vad_thold    = 0.6f;
//     float freq_thold   = 100.0f;

//     bool translate     = false;
//     bool no_fallback   = false;
//     bool print_special = false;
//     bool no_context    = true;
//     bool no_timestamps = false;
//     bool tinydiarize   = false;
//     bool save_audio    = false; // save audio to wav file
//     bool use_gpu       = true;
//     bool flash_attn    = false;

//     std::string language  = "en";
//     std::string model     = "out/models/ggml-base.en.bin";
//     std::string fname_out;
// };

void get_audio_data() {
    if (SDL_GetAudioStreamAvailable(stream) < AUDIO_CHUNK_SIZE) {
        return;
    }
    const int data_available = SDL_GetAudioStreamData(stream, (void *) audio_buffer.data(), sizeof(float) * audio_buffer.size());
    if (data_available == -1) {
        SDL_Log("Couldn't get audio stream data: %s", SDL_GetError());
        return;
    }
    audio_buffer_pos = data_available / sizeof(float);

    {
        std::lock_guard<std::mutex> lock(audio_buffer_for_speech_recognition_mutex);
        memcpy(
            audio_buffer_for_speech_recognition.data() + audio_buffer_for_speech_recognition_pos,
            audio_buffer.data(),
            sizeof(float) * audio_buffer_pos
        );
        audio_buffer_for_speech_recognition_pos += audio_buffer_pos;
    }
    // SDL_Log("Got audio stream data: %d bytes, buffer_size in bytes: %lu", data_available, audio_buffer.size() * sizeof(float));
    // wavWriter.write(audio_buffer.data(), data_available / sizeof(float));
}

whisper_context* setup_whisper() {
    // TODO: Parse whisper params and model from command line arguments
    struct whisper_context_params cparams = whisper_context_default_params();
    std::string model = "out/models/ggml-base.en.bin";

    prompt_tokens_for_speech_recognition.clear();

    struct whisper_context *ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
    if (!ctx) {
        SDL_Log("Couldn't initialize whisper context");
        return nullptr;
    }
    return ctx;
}

void run_whisper() {
    if (audio_buffer_for_speech_recognition_pos < n_samples_step) {
        return;
    }
    const int n_samples_to_keep = std::min(n_samples_keep, audio_buffer_for_speech_recognition_old_pos);
    // SDL_Log("n_samples_to_keep: %d", n_samples_to_keep);
    audio_buffer_for_speech_recognition_combined.clear();
    memcpy(
        audio_buffer_for_speech_recognition_combined.data(),
        audio_buffer_for_speech_recognition_old.data() + audio_buffer_for_speech_recognition_old_pos - n_samples_to_keep,
        sizeof(float) * n_samples_to_keep
    );

    int audio_buffer_for_speech_recognition_combined_size = n_samples_to_keep;
    {
        std::lock_guard<std::mutex> lock(audio_buffer_for_speech_recognition_mutex);
        memcpy(
            audio_buffer_for_speech_recognition_combined.data() + n_samples_to_keep,
            audio_buffer_for_speech_recognition.data(),
            sizeof(float) * audio_buffer_for_speech_recognition_pos
        );

        memcpy(
            audio_buffer_for_speech_recognition_old.data(),
            audio_buffer_for_speech_recognition.data() + audio_buffer_for_speech_recognition_pos - n_samples_to_keep,
            sizeof(float) * n_samples_to_keep
        );

        audio_buffer_for_speech_recognition_combined_size += audio_buffer_for_speech_recognition_pos;
        audio_buffer_for_speech_recognition_old_pos = n_samples_to_keep;
        audio_buffer_for_speech_recognition_pos = 0;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.prompt_tokens = prompt_tokens_for_speech_recognition.data();

    if (whisper_full(whisper_ctx, wparams, audio_buffer_for_speech_recognition_combined.data(), audio_buffer_for_speech_recognition_combined_size) != 0) {
        SDL_Log("Failed to process audio");
        return;
    }

    prompt_tokens_for_speech_recognition.clear();


    const int n_segments = whisper_full_n_segments(whisper_ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char *text = whisper_full_get_segment_text(whisper_ctx, i);
        SDL_Log("%s", text);

        if (text_speech_recognition.size() >= text_speech_recognition_size) {
            text_speech_recognition.pop_front();
        }

        text_speech_recognition.push_back(text);

        const int token_count = whisper_full_n_tokens(whisper_ctx, i);
        for (int j = 0; j < token_count; ++j) {
            prompt_tokens_for_speech_recognition.push_back(whisper_full_get_token_id(whisper_ctx, i, j));
        }
    }
}

void run_function_in_loop(void (*func)()) {
    while (true) {
        func();
        SDL_Delay(1);
    }
}

int run_function_sdl(void *ptr) {
    void (*func)() = (void (*)()) ptr;
    run_function_in_loop(func);
    return 0;
}

int get_window_device_pixel_ratio() {
    int wp, hp;
    SDL_GetWindowSizeInPixels(window, &wp, &hp);

    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    return wp / w;
}

static void PrintCameraSpecs(SDL_CameraID camera_id)
{
    SDL_CameraSpec **specs = SDL_GetCameraSupportedFormats(camera_id, NULL);
    if (specs) {
        int i;

        SDL_Log("Available formats:\n");
        for (i = 0; specs[i]; ++i) {
            const SDL_CameraSpec *s = specs[i];
            SDL_Log("    %dx%d %.2f FPS %s\n", s->width, s->height, (float)s->framerate_numerator / s->framerate_denominator, SDL_GetPixelFormatName(s->format));
        }
        SDL_free(specs);
    }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {

    SDL_SetAppMetadata(APP_NAME, APP_VERSION.c_str(), APP_IDENTIFIER);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_CAMERA)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window = SDL_CreateWindow(WINDOW_TITLE.c_str(), WIDTH, HEIGHT, window_flags);
    if (!window) {
        SDL_Log("Couldn't create window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);
    if (renderer == nullptr) {
        SDL_Log("Error: SDL_CreateRenderer(): %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("Renderer: %s", SDL_GetRendererName(renderer));

    SDL_Log("SDL window created, %p", window);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);


    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    SDL_Log("Imgui context created");
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ioRef = &io;
    SDL_Log("Imgui context saved to ioRef");

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    SDL_Log("Imgui style set");

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    int window_device_pixel_ratio = get_window_device_pixel_ratio();
    SDL_Log("Device pixel ratio: %d", window_device_pixel_ratio);
    ImGui::GetIO().FontGlobalScale = 1.f / window_device_pixel_ratio;


    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    // - Our Emscripten build process allows embedding fonts to be accessible at runtime from the "fonts/" folder. See Makefile.emscripten for details.
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);

    // TODO: if font not found, log and skip. Don't crash.
    // TODO: embed fonts in source code: https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#loading-font-data-embedded-in-source-code
    // TODO: Add support for emojis.
    // font = io.Fonts->AddFontFromFileTTF("./static/fonts/SF-Pro.ttf", 20.0f);
    font = io.Fonts->AddFontFromFileTTF("out/fonts/OpenSans-Regular.ttf", 16.0f * window_device_pixel_ratio);
    IM_ASSERT(font != nullptr);


    // Add this for sharpening the default font
    // float SCALE = window_device_pixel_ratio;
    // ImFontConfig cfg;
    // cfg.SizePixels = 13 * SCALE;
    // ImGui::GetIO().Fonts->AddFontDefault(&cfg);


    // Setup audio stream
    SDL_AudioSpec audio_spec;
    SDL_zero(audio_spec);

    // NOTE: Do not change the spec format to anything other than F32LE
    audio_spec.freq = WHISPER_SAMPLE_RATE;
    audio_spec.format = SDL_AUDIO_F32LE;
    audio_spec.channels = 1;

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &audio_spec, NULL, NULL);
    if (!stream) {
        SDL_Log("Couldn't create audio stream: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_AudioDeviceID dev_id = SDL_GetAudioStreamDevice(stream);
    if (!dev_id) {
        SDL_Log("Couldn't get audio stream device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    const char *device_name = SDL_GetAudioDeviceName(dev_id);
    SDL_Log("Got audio stream: freq: %d, channels: %d, format: %d", audio_spec.freq, audio_spec.channels, audio_spec.format);
    /* SDL_OpenAudioDeviceStream starts the device paused. You have to tell it to start! */
    SDL_ResumeAudioStreamDevice(stream);

    int devcount = 0;
    SDL_CameraID *devices = SDL_GetCameras(&devcount);
    if (!devices) {
        SDL_Log("SDL_GetCameras failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (devcount == 0) {
        SDL_Log("No cameras found");
        return SDL_APP_FAILURE;
    } else {
        SDL_Log("Found %d cameras", devcount);
        // TODO: Fix this later if needed
        // choose the first device as the camera
        camera_id = devices[0];
        PrintCameraSpecs(camera_id);
    }
    SDL_free(devices);


    // try 30 FPS
    spec.framerate_numerator = 30;
    spec.framerate_denominator = 1;
    camera = SDL_OpenCamera(camera_id, &spec);
    if (!camera) {
        SDL_Log("Failed to open camera device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Get current date/time for filename
    // time_t now = time(0);
    // char buffer[80];
    // strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localtime(&now));
    // audio_filename = std::string(buffer) + ".wav";

    // wavWriter.open(audio_filename, audio_spec.freq, 16, audio_spec.channels);

    whisper_ctx = setup_whisper();
    if (!whisper_ctx) {
        SDL_Log("Couldn't initialize whisper context");
        return SDL_APP_FAILURE;
    }

    // TODO: Maybe don't do this and run_whisper in the main thread?
    // I have seen frame rates dropping when running whisper in the main thread
    // get_audio_data();
    // run_whisper();
    SDL_DetachThread(
        SDL_CreateThread(run_function_sdl, "get_audio_data_loop", (void *) get_audio_data)
    );
    SDL_DetachThread(
        SDL_CreateThread(run_function_sdl, "run_whisper_loop", (void *) run_whisper)
    );

    SDL_Log("SDL_AppInit complete");
    return SDL_APP_CONTINUE;  /* carry on with the program! */

}


/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    // Poll and handle events (inputs, window resize, etc.)
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
    // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
    ImGui_ImplSDL3_ProcessEvent(event);
    // SDL_Log("SDL_AppEvent called with event type %d", event->type);
    if (event->type == SDL_EVENT_QUIT) {
        SDL_Log("SDL_EVENT_QUIT event received");
        return SDL_APP_SUCCESS;  /* end the program, reporting success to the OS. */
    } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(window)) {
        SDL_Log("SDL_EVENT_WINDOW_CLOSE_REQUESTED event received");
        return SDL_APP_SUCCESS;  /* end the program, reporting success to the OS. */
    } else if (event->type == SDL_EVENT_CAMERA_DEVICE_APPROVED) {
        SDL_Log("Camera approved!");
        SDL_CameraSpec camera_spec;
        SDL_GetCameraFormat(camera, &camera_spec);
        float fps = 0;
        if (camera_spec.framerate_denominator != 0) {
            fps = (float)camera_spec.framerate_numerator / (float)camera_spec.framerate_denominator;
        }
        SDL_Log("Camera Spec: %dx%d %.2f FPS %s",
                camera_spec.width, camera_spec.height, fps, SDL_GetPixelFormatName(camera_spec.format));
    } else if (event->type == SDL_EVENT_CAMERA_DEVICE_DENIED) {
        // TODO: Debug this. This event is not being triggered when I deny camera access
        // This might just be a MacOS issue
        SDL_Log("Camera denied!");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Camera permission denied!", "User denied access to the camera!", window);
        return SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;  /* carry on with the program! */
}

void update_camera_frame() {
    if (!camera) {
        return;
    }
    SDL_Surface *next_frame = SDL_AcquireCameraFrame(camera, NULL);

    // There is no space for NULL in my home
    if (!next_frame) {
        return;
    }
    // SDL_Log("Got camera frame: %p", next_frame);
    if (current_frame && next_frame) {
        SDL_ReleaseCameraFrame(camera, current_frame);
    }
    current_frame = next_frame;

    if (!current_frame_texture) {
        SDL_Colorspace colorspace = SDL_GetSurfaceColorspace(current_frame);

        /* Create texture with appropriate format */
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, current_frame->format);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, current_frame->w);

        SDL_Log("Video stream width: %d, height: %d", current_frame->w, current_frame->h);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, current_frame->h);
        current_frame_texture = SDL_CreateTextureWithProperties(renderer, props);
        SDL_DestroyProperties(props);
        if (!current_frame_texture) {
            SDL_Log("Couldn't create texture: %s", SDL_GetError());
            return;
        }
    }
    // TODO: Docs say the below statement
    // This is a fairly slow function, intended for use with static textures that do not change often.
    SDL_UpdateTexture(current_frame_texture, NULL, current_frame->pixels, current_frame->pitch);
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SDL_Log("SDL_AppQuit called with result %d, cleaning up", result);
    whisper_free(whisper_ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    // wavWriter.close();
    SDL_DestroyAudioStream(stream);
    SDL_ReleaseCameraFrame(camera, current_frame);
    SDL_CloseCamera(camera);
    SDL_DestroyTexture(current_frame_texture);
    /* SDL will clean up the window/renderer for us. */
}

void show_current_state() {
    // Note: https://pthom.github.io/imgui_manual_online/manual/imgui_manual.html
    // This website is great for learning how to use ImGui
    // Kudos to the author Emscripten, and webassembly.
    const int window_flags = ImGuiWindowFlags_NoResize | \
        ImGuiWindowFlags_NoMove | \
        ImGuiWindowFlags_NoSavedSettings | \
        ImGuiWindowFlags_NoBackground;

    const ImVec2 content_region = {ioRef->DisplaySize.x, ioRef->DisplaySize.y};
    const ImVec2 window_size = ImVec2(content_region.x / 2 - PADDING, content_region.y - PADDING * 2);
    ImGui::SetNextWindowSize(window_size);

    // Window on the right side of the screen
    const ImVec2 window_postition = ImVec2(ioRef->DisplaySize.x - window_size.x - PADDING, PADDING);
    ImGui::SetNextWindowPos(window_postition);
    ImGui::Begin(
        "Current State",
        nullptr,
        window_flags
    );
    ImGui::TextWrapped("Application version: %s", APP_VERSION.c_str());
    ImGui::TextWrapped("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ioRef->Framerate, ioRef->Framerate);
    ImGui::TextWrapped("Application window size: %.0f x %.0f", ioRef->DisplaySize.x, ioRef->DisplaySize.y);
    ImVec2 available_size = ImGui::GetContentRegionAvail();
    // TODO: Add a little padding in the width
    video_stream_width = available_size.x;

    // SDL_Log("Available size: %f, %f", available_size.x, available_size.y);
    if (ImGui::BeginTabBar("Current State Tab Bar", ImGuiTabBarFlags_None)) {
        // SDL_Log("GetContentRegionMax: %f, %f", ImGui::GetContentRegionMax().x, ImGui::GetContentRegionMax().y);
        if (ImGui::BeginTabItem("Video Stream")) {
            ImGui::BeginChild(
                "Video Stream",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_None,
                ImGuiWindowFlags_None
            );
            // get current date time
            time_t now = time(0);
            char buffer[80];
            strftime(buffer, sizeof(buffer), "Today: %Y-%m-%d %H:%M:%S", localtime(&now));
            ImGui::SeparatorText(buffer);
            int video_stream_height = (int) ((float) current_frame->h * video_stream_width / current_frame->w);
            if (current_frame_texture != NULL) {
                ImGui::Image((ImTextureID)(intptr_t) current_frame_texture, ImVec2(video_stream_width, video_stream_height));
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Audio Stream")) {
            ImGui::BeginChild(
                "Audio Stream",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_None,
                ImGuiWindowFlags_AlwaysVerticalScrollbar
            );
            std::string text_in_queue = "";
            for (const auto &text : text_speech_recognition) {
                text_in_queue += text + "\n";
            }
            ImGui::TextWrapped(text_in_queue.c_str());
            // scroll to the bottom
            // TODO: Since show_current_state() is called every frame, this will scroll to the bottom every frame.
            // This causes issues when the user tries to scroll up to see the previous text.
            // ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
    ImGui::End();
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
    {
        SDL_Delay(10);
        return SDL_APP_CONTINUE;
    }
    SDL_RenderClear(renderer);
    // Start the Dear ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
    // if (show_demo_window)
    //     ImGui::ShowDemoWindow(&show_demo_window);

    // Do not return void from below functions, change their definitions
    // return SDL_AppResult
    show_current_state();
    update_camera_frame();

    // Rendering
    ImGui::Render();
    SDL_SetRenderScale(renderer, ioRef->DisplayFramebufferScale.x, ioRef->DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    return SDL_APP_CONTINUE;  /* carry on with the program! */
}

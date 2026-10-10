#pragma once
// P0: Screensaver engine — khung chung cho các mode.
// Mỗi mode implement init/update/render/shutdown.
// P0: 2 clock có sẵn. P1: starfield. P2: fire. P3: picture frame.
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface;
#include <SDL2/SDL_ttf.h>

namespace RomCloud {

class UIManager;

enum class SaverMode {
    CLOCK_FLIP = 0,   // Đồng hồ thẻ lật (có sẵn)
    CLOCK_QLOCK = 1,  // Đồng hồ QlockTwo tiếng Việt (có sẵn)
    STARFIELD = 2,    // P1: sao bay
    MATRIX = 3,       // P2-Tai: Matrix rain (thay Fire)
    PICTURE = 4,      // P3: picture frame
};

struct SaverModeInfo {
    SaverMode id;
    std::string name;        // Tên hiển thị
    std::string description; // Mô tả ngắn
    bool available;          // P0: chỉ clock available
};

class SaverEngine {
public:
    SaverEngine();
    ~SaverEngine();

    // Khởi tạo với UIManager và renderer (để tạo texture)
    void init(UIManager* ui, SDL_Renderer* renderer);
    void shutdown();

    // Chuyển mode
    void setMode(SaverMode mode);
    SaverMode getMode() const { return m_mode; }
    
    // Danh sách mode (cho menu chọn)
    std::vector<SaverModeInfo> getModes() const;

    // Vòng đời mode hiện tại
    void update(float dt);
    void render();
    
    // Input: trả về true nếu đã xử lý (không thoát), false nếu nên thoát saver
    bool handleInput(int button);

private:
    UIManager* m_ui = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_fbTexture = nullptr;  // texture 480x360 để upscale
    SaverMode m_mode = SaverMode::CLOCK_FLIP;
    bool m_initialized = false;

    // P1: Starfield — framebuffer 480x360 RGBA
    static const int FB_W = 480;
    static const int FB_H = 360;
    uint32_t m_framebuffer[FB_W * FB_H];
    
    struct Star {
        float x, y, z;  // x,y in [-1,1], z in (0,1]
    };
    static const int STAR_COUNT = 250;
    Star m_stars[STAR_COUNT];
    float m_starSpeed = 0.6f;  // tốc độ bay (z units/giây)
    
    void starfieldInit();
    void starfieldUpdate(float dt);
    void starfieldRender();
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    // P2-Tai: Matrix rain (thay Fire)
    static const int MATRIX_COLS = 40;
    static const int MATRIX_GLYPHS = 32;
    SDL_Texture* m_matrixGlyphs[MATRIX_GLYPHS];
    struct MatrixCol {
        float y;        // vị trí đầu (pixels)
        float speed;    // pixels/giây
        int len;        // độ dài vệt (số ký tự)
        int chars[32];  // ký tự trong cột (index vào glyphs)
    };
    MatrixCol m_matrixCols[MATRIX_COLS];
    int m_matrixCharW = 12;
    int m_matrixCharH = 20;
    void matrixInit();
    void matrixUpdate(float dt);
    void matrixRender();
    void matrixFreeGlyphs();

    // P3: Picture frame
    std::vector<std::string> m_photos;  // ảnh hiện (cho slideshow)
    std::vector<std::string> m_allPhotos;  // tất cả ảnh (cho chọn)
    int m_photoIdx = 0;                  // ảnh đang hiện
    SDL_Texture* m_photoTex = nullptr;   // texture ảnh hiện tại
    SDL_Texture* m_photoNextTex = nullptr; // texture ảnh kế (cho transition)
    SDL_Surface* m_photoSurf = nullptr;  // surface ảnh hiện tại (pixel access)
    SDL_Surface* m_photoNextSurf = nullptr; // surface ảnh kế
    uint32_t* m_photoPixels = nullptr;   // pixels 480x360 của ảnh hiện tại
    uint32_t* m_photoNextPixels = nullptr; // pixels 480x360 của ảnh kế
    float m_photoTimer = 0;              // đếm thời gian hiện ảnh
    float m_photoDuration = 10.0f;       // giây mỗi ảnh (setting)
    bool m_photoShuffle = true;
    std::string m_photoDir = "/mnt/SDCARD/Apps/RomCloud/screensaver/photos";
    
    // Transition
    int m_transType = -1;  // -1=random, 0-23=cụ thể (cài đặt người dùng)
    int m_activeTransType = 0; // hiệu ứng đang chạy
    float m_transProgress = 0;  // 0→1 trong 0.8s
    bool m_transActive = false;
    static constexpr float TRANS_DURATION = 0.8f;
    
    void photoInit();
    void photoUpdate(float dt);
    void photoRender();
    void scanPhotos();
    bool loadPhoto(const std::string& path, SDL_Texture** outTex);
    uint32_t* textureToPixels(SDL_Texture* tex);
    void nextPhoto();
    void prevPhoto();
    void hideCurrentPhoto();
    void startTransition();
    void applyTransition(uint32_t* out, uint32_t* oldFb, uint32_t* newFb, float progress);
    
    // P3: Settings menu (SELECT)
    bool m_settingsOpen = false;
    int m_settingsSel = 0;
    std::vector<std::string> m_hiddenPhotos;
    TTF_Font* m_font = nullptr;
    TTF_Font* m_fontTitle = nullptr;
    // P3-Tai: chọn ảnh trên máy
    bool m_photoListOpen = false;
    int m_photoListSel = 0;
    int m_photoListScroll = 0;
    void toggleSettings();
    void settingsInput(int button);
    void drawText(const char* text, int x, int y, uint8_t r, uint8_t g, uint8_t b, bool centered = false, TTF_Font* customFont = nullptr);
    int getTextWidth(const char* text, TTF_Font* customFont = nullptr);
    const char* getPhotoName(const std::string& path);
    bool isPhotoHidden(const std::string& path);
};

} // namespace RomCloud

// P0: Screensaver engine implementation.
// P1: Starfield — sao bay về phía người xem.
// P2: Fire — lửa lò sưởi.
// P3: Picture frame — slideshow ảnh.
#include "SaverEngine.h"
#include "../ui/UIManager.h"
#include "../input/InputManager.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <random>
#include <ctime>
#include <thread>

namespace RomCloud {

SaverEngine::SaverEngine() {
    // Khởi tạo mảng glyphs về nullptr (tránh crash khi free)
    for (int i = 0; i < MATRIX_GLYPHS; i++) {
        m_matrixGlyphs[i] = nullptr;
    }
}
SaverEngine::~SaverEngine() { shutdown(); }

void SaverEngine::init(UIManager* ui, SDL_Renderer* renderer) {
    m_ui = ui;
    m_renderer = renderer;
    // Tạo texture 480x360 để upscale fullscreen
    if (m_renderer) {
        m_fbTexture = SDL_CreateTexture(m_renderer,
            SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
            FB_W, FB_H);
    }
    // P3: load font cho settings menu — cùng thứ tự chuẩn với UIManager
    // (NotoSans-Regular của app trước, font hệ sau) để tiếng Việt không vỡ.
    const char* fontCandidates[] = {
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/NotoSans-Regular.ttf",
        "assets/fonts/NotoSans-Regular.ttf",
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/DejaVuSans.ttf",
        "assets/fonts/DejaVuSans.ttf",
        "/rom/usr/trimui/res/full.ttf",
        "/usr/trimui/res/full.ttf",
        "/usr/trimui/res/regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
    };
    for (const char* fp : fontCandidates) {
        if (!m_font) m_font = TTF_OpenFont(fp, 22);
        if (!m_fontTitle) m_fontTitle = TTF_OpenFont(fp, 26);
        if (m_font && m_fontTitle) break;
    }
    starfieldInit();
    m_initialized = true;
}

void SaverEngine::shutdown() {
    joinPreload();  // dừng hẳn thread giải mã ảnh kế trước khi dọn
    stopThumbWorker();  // dừng thread thumbnail lưới chọn ảnh
    if (m_fbTexture) {
        SDL_DestroyTexture(m_fbTexture);
        m_fbTexture = nullptr;
    }
    matrixFreeGlyphs();
    if (m_photoTex) { SDL_DestroyTexture(m_photoTex); m_photoTex = nullptr; }
    if (m_photoNextTex) { SDL_DestroyTexture(m_photoNextTex); m_photoNextTex = nullptr; }
    if (m_photoPixels) { delete[] m_photoPixels; m_photoPixels = nullptr; }
    if (m_photoNextPixels) { delete[] m_photoNextPixels; m_photoNextPixels = nullptr; }
    if (m_font) { TTF_CloseFont(m_font); m_font = nullptr; }
    if (m_fontTitle) { TTF_CloseFont(m_fontTitle); m_fontTitle = nullptr; }
    m_initialized = false;
    m_ui = nullptr;
    m_renderer = nullptr;
}

void SaverEngine::setMode(SaverMode mode) {
    m_mode = mode;
    if (m_ui) {
        if (mode == SaverMode::CLOCK_FLIP) {
            m_ui->saverSetClockStyle(0);
        } else if (mode == SaverMode::CLOCK_QLOCK) {
            m_ui->saverSetClockStyle(2);
        } else if (mode == SaverMode::STARFIELD) {
            starfieldInit();
        } else if (mode == SaverMode::MATRIX) {
            matrixInit();
        } else if (mode == SaverMode::PICTURE) {
            photoInit();
        }
    }
}

std::vector<SaverModeInfo> SaverEngine::getModes() const {
    return {
        {SaverMode::CLOCK_FLIP, "Đồng hồ lật", "Thẻ lật kiểu Fliqlo", true},
        {SaverMode::CLOCK_QLOCK, "QlockTwo", "Đồng hồ chữ tiếng Việt", true},
        {SaverMode::STARFIELD, "Starfield", "Sao bay về phía bạn", true},
        {SaverMode::MATRIX, "Matrix", "Chữ rơi kiểu Matrix", true},
        {SaverMode::PICTURE, "Picture frame", "Ảnh slideshow", true},
    };
}

void SaverEngine::update(float dt) {
    if (m_mode == SaverMode::STARFIELD) {
        starfieldUpdate(dt);
    } else if (m_mode == SaverMode::MATRIX) {
        matrixUpdate(dt);
    } else if (m_mode == SaverMode::PICTURE) {
        photoUpdate(dt);
    }
}

void SaverEngine::render() {
    if (!m_ui || !m_initialized) return;
    if (m_mode == SaverMode::CLOCK_FLIP || m_mode == SaverMode::CLOCK_QLOCK) {
        m_ui->saverRenderClock();
    } else if (m_mode == SaverMode::STARFIELD) {
        starfieldRender();
    } else if (m_mode == SaverMode::MATRIX) {
        matrixRender();
    } else if (m_mode == SaverMode::PICTURE) {
        photoRender();
    }
}

bool SaverEngine::handleInput(int button) {
    // Button enum (InputManager.h): UP=0, DOWN=1, LEFT=2, RIGHT=3, A=4, B=5, X=6, Y=7, L1=8, R1=9, START=12, SELECT=13
    const int BTN_UP = 0;
    const int BTN_DOWN = 1;
    const int BTN_LEFT = 2;
    const int BTN_RIGHT = 3;
    const int BTN_A = 4;
    const int BTN_B = 5;
    const int BTN_X = 6;
    const int BTN_Y = 7;
    const int BTN_L1 = 8;
    const int BTN_R1 = 9;
    const int BTN_START = 12;  // Tai: START hiện/ẩn footer khi xem ảnh
    const int BTN_SELECT = 13; // Tai: SELECT mở cài đặt ảnh (từ 2026-10-10)
    
    // B: thoát (duy nhất)
    if (button == BTN_B) {
        // Trong settings/list: B đóng menu, không thoát saver
        if (m_mode == SaverMode::PICTURE && (m_settingsOpen || m_photoListOpen)) {
            if (m_photoListOpen) m_photoListOpen = false;
            else m_settingsOpen = false;
            return true;
        }
        return false;
    }
    
    // P3: Picture settings đang mở — D-pad điều khiển settings, KHÔNG chuyển mode
    if (m_mode == SaverMode::PICTURE && (m_settingsOpen || m_photoListOpen)) {
        settingsInput(button);
        return true;
    }
    
    // Tai: D-pad trái/phải chuyển MODE (4 modes)
    if (button == BTN_LEFT || button == BTN_RIGHT) {
        int curGroup = 0;
        if (m_mode == SaverMode::CLOCK_FLIP || m_mode == SaverMode::CLOCK_QLOCK) curGroup = 0;
        else if (m_mode == SaverMode::STARFIELD) curGroup = 1;
        else if (m_mode == SaverMode::MATRIX) curGroup = 2;
        else if (m_mode == SaverMode::PICTURE) curGroup = 3;
        
        int dir = (button == BTN_RIGHT) ? 1 : -1;
        int newGroup = (curGroup + dir + 4) % 4;
        
        if (newGroup == 0) {
            // Về clock: giữ style hiện tại nếu đang ở clock, không thì mặc định flip
            if (m_mode != SaverMode::CLOCK_FLIP && m_mode != SaverMode::CLOCK_QLOCK)
                setMode(SaverMode::CLOCK_FLIP);
        }
        else if (newGroup == 1) setMode(SaverMode::STARFIELD);
        else if (newGroup == 2) setMode(SaverMode::MATRIX);
        else if (newGroup == 3) setMode(SaverMode::PICTURE);
        return true;
    }
    
    // Tai: D-pad lên/xuống đổi STYLE trong mode
    if (button == BTN_UP || button == BTN_DOWN) {
        // Trong CLOCK mode: đổi giữa flip và qlock
        if (m_mode == SaverMode::CLOCK_FLIP || m_mode == SaverMode::CLOCK_QLOCK) {
            setMode(m_mode == SaverMode::CLOCK_FLIP ? SaverMode::CLOCK_QLOCK : SaverMode::CLOCK_FLIP);
        }
        // Các mode khác: chưa có style → không tác dụng
        return true;
    }
    
    // P3: Picture mode
    if (m_mode == SaverMode::PICTURE) {
        if (button == BTN_START) {
            // Tai 2026-10-10: START hiện/ẩn footer gợi ý khi xem ảnh
            m_footerVisible = !m_footerVisible;
            return true;
        }
        if (button == BTN_SELECT) {
            toggleSettings();
            return true;
        }
        if (button == BTN_A) {
            hideCurrentPhoto();
            return true;
        }
        if (button == BTN_L1) {
            prevPhoto();
            return true;
        }
        if (button == BTN_R1) {
            nextPhoto();
            return true;
        }
        return true;  // nút khác: không tác dụng
    }
    
    // Các mode khác, nút không gán: không tác dụng
    return true;
}

// ---------------------------------------------------------------------------
// P1: Starfield
// ---------------------------------------------------------------------------

void SaverEngine::starfieldInit() {
    for (int i = 0; i < STAR_COUNT; i++) {
        m_stars[i].x = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        m_stars[i].y = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        m_stars[i].z = (rand() / (float)RAND_MAX);
        if (m_stars[i].z < 0.05f) m_stars[i].z = 0.05f;
    }
    // Xóa framebuffer
    memset(m_framebuffer, 0, sizeof(m_framebuffer));
}

void SaverEngine::starfieldUpdate(float dt) {
    for (int i = 0; i < STAR_COUNT; i++) {
        m_stars[i].z -= m_starSpeed * dt;
        if (m_stars[i].z <= 0.02f) {
            // Bay qua mặt → respawn ở xa
            m_stars[i].x = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
            m_stars[i].y = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
            m_stars[i].z = 1.0f;
        }
    }
}

void SaverEngine::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;
    // ABGR8888: A ở byte cao nhất
    m_framebuffer[y * FB_W + x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

void SaverEngine::starfieldRender() {
    if (!m_renderer || !m_fbTexture) return;
    
    // Xóa nền đen
    memset(m_framebuffer, 0, sizeof(m_framebuffer));
    
    const float focal = 220.0f;  // tiêu cự projection
    const int cx = FB_W / 2;
    const int cy = FB_H / 2;
    
    for (int i = 0; i < STAR_COUNT; i++) {
        const Star& s = m_stars[i];
        // Project 3D → 2D
        float sx = cx + (s.x / s.z) * focal;
        float sy = cy + (s.y / s.z) * focal;
        int ix = (int)sx;
        int iy = (int)sy;
        
        if (ix < 0 || ix >= FB_W || iy < 0 || iy >= FB_H) continue;
        
        // Độ sáng theo khoảng cách: gần → sáng
        float brightness = 1.0f - s.z;  // 0 (xa) → 1 (gần)
        if (brightness < 0) brightness = 0;
        if (brightness > 1) brightness = 1;
        uint8_t b = (uint8_t)(brightness * 255);
        
        // Sao gần thì vẽ lớn hơn (2x2 hoặc 3x3)
        if (s.z < 0.25f) {
            // Sao rất gần: 2x2
            setPixel(ix, iy, b, b, b);
            setPixel(ix+1, iy, b, b, b);
            setPixel(ix, iy+1, b, b, b);
            setPixel(ix+1, iy+1, b, b, b);
        } else if (s.z < 0.5f) {
            // Sao trung bình: vẽ thêm điểm sáng ngang (vệt motion)
            setPixel(ix, iy, b, b, b);
            // Vệt kéo dài theo hướng từ tâm
            float dx = s.x / s.z;
            float dy = s.y / s.z;
            float len = sqrtf(dx*dx + dy*dy);
            if (len > 0.01f) {
                int tx = ix - (int)(dx / len * 3);
                int ty = iy - (int)(dy / len * 3);
                setPixel(tx, ty, b/2, b/2, b/2);
            }
        } else {
            setPixel(ix, iy, b, b, b);
        }
    }
    
    // Upload framebuffer lên texture (1 lần/frame)
    SDL_UpdateTexture(m_fbTexture, nullptr, m_framebuffer, FB_W * sizeof(uint32_t));
    
    // Vẽ texture fullscreen (upscale 480x360 → 1024x768)
    SDL_RenderCopy(m_renderer, m_fbTexture, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// P2-Tai: Matrix rain (thay Fire)
// ---------------------------------------------------------------------------

void SaverEngine::matrixFreeGlyphs() {
    for (int i = 0; i < MATRIX_GLYPHS; i++) {
        if (m_matrixGlyphs[i]) {
            SDL_DestroyTexture(m_matrixGlyphs[i]);
            m_matrixGlyphs[i] = nullptr;
        }
    }
}

void SaverEngine::matrixInit() {
    matrixFreeGlyphs();
    
    // Pre-render glyphs: ký tự kiểu Matrix
    const char* charset = "0123456789ABCDEFXYZ<>*+-=|";
    // Cùng thứ tự chuẩn với UIManager (NotoSans trước).
    TTF_Font* mfont = nullptr;
    const char* fontCandidates[] = {
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/NotoSans-Regular.ttf",
        "assets/fonts/NotoSans-Regular.ttf",
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/DejaVuSans.ttf",
        "assets/fonts/DejaVuSans.ttf",
        "/rom/usr/trimui/res/full.ttf",
        "/usr/trimui/res/full.ttf",
        "/usr/trimui/res/regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    };
    for (const char* fp : fontCandidates) {
        mfont = TTF_OpenFont(fp, 18);
        if (mfont) break;
    }
    if (!mfont && m_font) mfont = m_font;  // fallback
    
    int len = strlen(charset);
    for (int i = 0; i < MATRIX_GLYPHS; i++) {
        char c[2] = {charset[i % len], 0};
        SDL_Color col = {0, 255, 0, 255};
        SDL_Surface* surf = nullptr;
        if (mfont) surf = TTF_RenderUTF8_Blended(mfont, c, col);
        if (surf && m_renderer) {
            m_matrixGlyphs[i] = SDL_CreateTextureFromSurface(m_renderer, surf);
            SDL_FreeSurface(surf);
        } else {
            m_matrixGlyphs[i] = nullptr;
        }
    }
    if (mfont && mfont != m_font) TTF_CloseFont(mfont);
    
    // Khởi tạo cột
    for (int i = 0; i < MATRIX_COLS; i++) {
        m_matrixCols[i].y = (rand() % 600) - 200;  // random start
        m_matrixCols[i].speed = 80 + (rand() % 120);  // 80-200 px/s
        m_matrixCols[i].len = 8 + (rand() % 16);  // 8-24 ký tự
        for (int j = 0; j < 32; j++) {
            m_matrixCols[i].chars[j] = rand() % MATRIX_GLYPHS;
        }
    }
}

void SaverEngine::matrixUpdate(float dt) {
    for (int i = 0; i < MATRIX_COLS; i++) {
        m_matrixCols[i].y += m_matrixCols[i].speed * dt;
        // Khi đầu cột ra khỏi màn hình, reset lên trên
        float tailY = m_matrixCols[i].y - m_matrixCols[i].len * m_matrixCharH;
        if (tailY > 768) {  // 768 = chiều cao màn hình device
            m_matrixCols[i].y = -(rand() % 200);
            m_matrixCols[i].speed = 80 + (rand() % 120);
            m_matrixCols[i].len = 8 + (rand() % 16);
            // Đổi ký tự ngẫu nhiên
            for (int j = 0; j < 32; j++) {
                if ((rand() % 10) == 0) {
                    m_matrixCols[i].chars[j] = rand() % MATRIX_GLYPHS;
                }
            }
        }
        // Thỉnh thoảng đổi ký tự ở đầu (nhấp nháy)
        if ((rand() % 20) == 0) {
            m_matrixCols[i].chars[0] = rand() % MATRIX_GLYPHS;
        }
    }
}

void SaverEngine::matrixRender() {
    if (!m_renderer) return;
    
    // Nền đen
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    
    // Vẽ từng cột
    for (int c = 0; c < MATRIX_COLS; c++) {
        int x = c * (1024 / MATRIX_COLS) + 4;  // 1024 = rộng màn hình
        MatrixCol& col = m_matrixCols[c];
        
        for (int j = 0; j < col.len && j < 32; j++) {
            int y = (int)(col.y - j * m_matrixCharH);
            if (y < -m_matrixCharH || y > 768) continue;
            
            int gi = col.chars[j];
            if (gi < 0 || gi >= MATRIX_GLYPHS || !m_matrixGlyphs[gi]) continue;
            
            // Đầu sáng nhất, mờ dần về đuôi
            Uint8 alpha;
            Uint8 r, g, b;
            if (j == 0) {
                // Đầu: trắng xanh sáng
                r = 200; g = 255; b = 200; alpha = 255;
            } else {
                // Trail: xanh lá mờ dần
                float f = 1.0f - (float)j / col.len;
                r = 0; g = (Uint8)(255 * f); b = 0;
                alpha = (Uint8)(200 * f + 55);
            }
            
            SDL_SetTextureColorMod(m_matrixGlyphs[gi], r, g, b);
            SDL_SetTextureAlphaMod(m_matrixGlyphs[gi], alpha);
            
            int w, h;
            SDL_QueryTexture(m_matrixGlyphs[gi], nullptr, nullptr, &w, &h);
            SDL_Rect dst = {x, y, w, h};
            SDL_RenderCopy(m_renderer, m_matrixGlyphs[gi], nullptr, &dst);
        }
    }
    
    // Reset color mod
    for (int i = 0; i < MATRIX_GLYPHS; i++) {
        if (m_matrixGlyphs[i]) {
            SDL_SetTextureColorMod(m_matrixGlyphs[i], 255, 255, 255);
            SDL_SetTextureAlphaMod(m_matrixGlyphs[i], 255);
        }
    }
}

// ---------------------------------------------------------------------------
// P3: Picture frame
// ---------------------------------------------------------------------------

// Quét 1 thư mục, nạp các file jpg/jpeg/png vào m_allPhotos (và
// m_photos nếu không bị ẩn). Thư mục không có thì bỏ qua.
void SaverEngine::scanPhotoDir(const std::string& targetDir) {
    DIR* dir = opendir(targetDir.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        // Chỉ jpg/jpeg/png (bỏ qua webp nếu decode fail)
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.size() >= 4) {
            std::string ext = low.substr(low.size() - 4);
            if (ext == ".jpg" || ext == ".png" ||
                (low.size() >= 5 && low.substr(low.size() - 5) == ".jpeg")) {
                std::string full = targetDir + "/" + name;
                m_allPhotos.push_back(full);
                if (!isPhotoHidden(full)) {
                    m_photos.push_back(full);
                }
            }
        }
    }
    closedir(dir);
}

void SaverEngine::scanPhotos() {
    m_allPhotos.clear();
    m_photos.clear();
    // Tai 2026-10-10: nguồn ảnh chính là thư mục Pictures ở gốc thẻ
    // (chép ảnh từ máy tính vào đây là dùng được, không phải đào sâu
    // trong thư mục app); quét thêm thư mục photos cũ trong app để
    // không mất ảnh đã có. Bật/tắt từng ảnh ở Cài đặt -> Chọn ảnh.
    mkdir("/mnt/SDCARD/Pictures", 0777);
    scanPhotoDir("/mnt/SDCARD/Pictures");
    {
        DIR* probe = opendir(m_photoDir.c_str());
        if (probe) {
            closedir(probe);
            scanPhotoDir(m_photoDir);
        } else {
            scanPhotoDir("screensaver/photos");
        }
    }

    
    static std::default_random_engine rng((unsigned)time(nullptr));
    if (m_photoShuffle) {
        std::shuffle(m_allPhotos.begin(), m_allPhotos.end(), rng);
        std::shuffle(m_photos.begin(), m_photos.end(), rng);
    } else {
        std::sort(m_allPhotos.begin(), m_allPhotos.end());
        std::sort(m_photos.begin(), m_photos.end());
    }
}

// Chỉ phần CPU của việc load ảnh: giải mã file gốc + scale về vừa
// 1024x768. Chạy được trong thread nền (không đụng renderer/texture —
// cùng quy ước với worker thumbnail của UIManager).
SDL_Surface* SaverEngine::loadPhotoSurface(const std::string& path, int maxW, int maxH) {
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return nullptr;
    
    // Scale về vừa khung maxW x maxH (giữ tỷ lệ)
    int sw = surf->w;
    int sh = surf->h;
    float scale = std::min((float)maxW / sw, (float)maxH / sh);
    if (scale > 1.0f) scale = 1.0f;
    int dw = (int)(sw * scale);
    int dh = (int)(sh * scale);
    
    SDL_Surface* scaled = nullptr;
    if (dw != sw || dh != sh) {
        scaled = SDL_CreateRGBSurface(0, dw, dh, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (scaled) {
            SDL_BlitScaled(surf, nullptr, scaled, nullptr);
            SDL_FreeSurface(surf);
            surf = scaled;
        }
    }
    return surf;
}

bool SaverEngine::loadPhoto(const std::string& path, SDL_Texture** outTex) {
    if (!m_renderer || !outTex) return false;
    SDL_Surface* surf = loadPhotoSurface(path, 1024, 768);
    if (!surf) return false;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return false;
    *outTex = tex;
    return true;
}

// P4: kích thread nền giải mã trước ảnh kế tiếp trong lúc ảnh hiện tại
// đang hiển thị, để tới lúc chuyển ảnh là có sẵn texture mà dùng.
void SaverEngine::kickPreloadNext() {
    if (m_photos.size() < 2) return;
    std::string path = m_photos[(m_photoIdx + 1) % (int)m_photos.size()];
    if (m_preloadThread.joinable() && m_preloadPath == path) return;
    joinPreload();
    m_preloadPath = path;
    m_preloadThread = std::thread([this, path]() {
        m_preloadSurf = loadPhotoSurface(path, 1024, 768);
    });
}

void SaverEngine::joinPreload() {
    if (m_preloadThread.joinable()) m_preloadThread.join();
    if (m_preloadSurf) { SDL_FreeSurface(m_preloadSurf); m_preloadSurf = nullptr; }
    m_preloadPath.clear();
}

// Lấy texture từ preload nếu đã giải mã đúng ảnh cần; chờ thread xong
// (thường đã xong từ lâu). Không khớp thì trả nullptr để nơi gọi load
// đồng bộ như cũ.
SDL_Texture* SaverEngine::takePreloadedTexture(const std::string& path) {
    if (!m_preloadThread.joinable() && !m_preloadSurf) return nullptr;
    if (m_preloadThread.joinable()) m_preloadThread.join();
    SDL_Texture* tex = nullptr;
    if (m_preloadSurf && m_preloadPath == path && m_renderer) {
        tex = SDL_CreateTextureFromSurface(m_renderer, m_preloadSurf);
    }
    if (m_preloadSurf) { SDL_FreeSurface(m_preloadSurf); m_preloadSurf = nullptr; }
    m_preloadPath.clear();
    return tex;
}

// ── Thumbnail cho lưới chọn ảnh ─────────────────────────────────────
// Thread nền giải mã ảnh nhỏ (208x156), main thread tạo texture khi
// vẽ lưới — cùng quy ước worker thumbnail của UIManager.
void SaverEngine::ensureThumbWorker() {
    if (m_thumbStarted) return;
    m_thumbStarted = true;
    m_thumbStop = false;
    m_thumbThread = std::thread([this]() {
        while (true) {
            std::string path;
            {
                std::unique_lock<std::mutex> lk(m_thumbMutex);
                m_thumbCv.wait(lk, [this]() {
                    return m_thumbStop || !m_thumbQueue.empty();
                });
                if (m_thumbStop) return;
                path = m_thumbQueue.front();
                m_thumbQueue.pop_front();
            }
            SDL_Surface* surf = loadPhotoSurface(path, 208, 156);
            {
                std::lock_guard<std::mutex> lk(m_thumbMutex);
                m_thumbReady.push_back({path, surf});
            }
        }
    });
}

void SaverEngine::stopThumbWorker() {
    if (!m_thumbStarted) return;
    {
        std::lock_guard<std::mutex> lk(m_thumbMutex);
        m_thumbStop = true;
    }
    m_thumbCv.notify_all();
    if (m_thumbThread.joinable()) m_thumbThread.join();
    m_thumbStarted = false;
    freeThumbs();
}

void SaverEngine::freeThumbs() {
    for (auto& kv : m_thumbTex) {
        if (kv.second) SDL_DestroyTexture(kv.second);
    }
    m_thumbTex.clear();
    m_thumbOrder.clear();
    std::lock_guard<std::mutex> lk(m_thumbMutex);
    for (auto& rs : m_thumbReady) {
        if (rs.second) SDL_FreeSurface(rs.second);
    }
    m_thumbReady.clear();
    m_thumbQueue.clear();
}

// Chuyển các surface đã giải mã xong thành texture (gọi ở thread vẽ).
void SaverEngine::pumpThumbs() {
    std::vector<std::pair<std::string, SDL_Surface*>> ready;
    {
        std::lock_guard<std::mutex> lk(m_thumbMutex);
        ready.swap(m_thumbReady);
    }
    for (auto& rs : ready) {
        if (!rs.second) continue;
        if (!m_renderer || m_thumbTex.count(rs.first)) {
            SDL_FreeSurface(rs.second);
            continue;
        }
        SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, rs.second);
        SDL_FreeSurface(rs.second);
        if (!tex) continue;
        // Đầy bộ nhớ đệm: đuổi thumbnail cũ nhất
        if ((int)m_thumbTex.size() >= THUMB_MAX && !m_thumbOrder.empty()) {
            auto old = m_thumbTex.find(m_thumbOrder.front());
            if (old != m_thumbTex.end()) {
                SDL_DestroyTexture(old->second);
                m_thumbTex.erase(old);
            }
            m_thumbOrder.erase(m_thumbOrder.begin());
        }
        m_thumbTex[rs.first] = tex;
        m_thumbOrder.push_back(rs.first);
    }
}

// Lấy texture thumbnail; chưa có thì xếp hàng cho thread nền giải mã
// và trả nullptr (ô ảnh hiện khung chờ).
SDL_Texture* SaverEngine::getThumb(const std::string& path) {
    auto it = m_thumbTex.find(path);
    if (it != m_thumbTex.end()) return it->second;
    ensureThumbWorker();
    {
        std::lock_guard<std::mutex> lk(m_thumbMutex);
        bool queued = std::find(m_thumbQueue.begin(), m_thumbQueue.end(), path) != m_thumbQueue.end();
        bool ready = false;
        for (auto& rs : m_thumbReady) {
            if (rs.first == path) { ready = true; break; }
        }
        if (!queued && !ready) {
            m_thumbQueue.push_back(path);
            m_thumbCv.notify_one();
        }
    }
    return nullptr;
}

// Tạo pixel buffer 480x360 từ texture (dùng cho transitions)
// Trả về con trỏ cần free bằng delete[]
uint32_t* SaverEngine::textureToPixels(SDL_Texture* tex) {
    if (!tex || !m_renderer) return nullptr;
    
    uint32_t* pixels = new uint32_t[FB_W * FB_H];
    
    // Render texture vào target tạm rồi đọc pixels
    SDL_Texture* target = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_TARGET, FB_W, FB_H);
    if (!target) { delete[] pixels; return nullptr; }
    
    SDL_Texture* oldTarget = SDL_GetRenderTarget(m_renderer);
    SDL_SetRenderTarget(m_renderer, target);
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, tex, nullptr, nullptr);
    
    SDL_Rect rect = {0, 0, FB_W, FB_H};
    SDL_RenderReadPixels(m_renderer, &rect, SDL_PIXELFORMAT_ABGR8888,
        pixels, FB_W * sizeof(uint32_t));
    
    SDL_SetRenderTarget(m_renderer, oldTarget);
    SDL_DestroyTexture(target);
    
    return pixels;
}

void SaverEngine::photoInit() {
    joinPreload();  // dọn preload của phiên xem ảnh trước (nếu có)
    scanPhotos();
    m_photoIdx = 0;
    m_photoTimer = 0;
    m_transActive = false;
    
    // Xóa texture cũ
    if (m_photoTex) { SDL_DestroyTexture(m_photoTex); m_photoTex = nullptr; }
    if (m_photoNextTex) { SDL_DestroyTexture(m_photoNextTex); m_photoNextTex = nullptr; }
    if (m_photoPixels) { delete[] m_photoPixels; m_photoPixels = nullptr; }
    if (m_photoNextPixels) { delete[] m_photoNextPixels; m_photoNextPixels = nullptr; }
    
    // Load ảnh đầu
    if (!m_photos.empty()) {
        loadPhoto(m_photos[0], &m_photoTex);
        m_photoPixels = textureToPixels(m_photoTex);
        kickPreloadNext();
    }
}

void SaverEngine::nextPhoto() {
    if (m_photos.empty()) return;
    m_photoIdx = (m_photoIdx + 1) % m_photos.size();
    startTransition();
}

void SaverEngine::prevPhoto() {
    if (m_photos.empty()) return;
    m_photoIdx = (m_photoIdx - 1 + m_photos.size()) % m_photos.size();
    startTransition();
}

void SaverEngine::hideCurrentPhoto() {
    if (m_photos.empty() || m_photoIdx >= (int)m_photos.size()) return;
    std::string cur = m_photos[m_photoIdx];
    // Thêm vào hidden
    m_hiddenPhotos.push_back(cur);
    // Xóa khỏi list hiện tại
    m_photos.erase(m_photos.begin() + m_photoIdx);
    if (m_photoIdx >= (int)m_photos.size()) m_photoIdx = 0;
    // Load lại ảnh
    if (m_photoTex) { SDL_DestroyTexture(m_photoTex); m_photoTex = nullptr; }
    if (m_photoPixels) { delete[] m_photoPixels; m_photoPixels = nullptr; }
    if (!m_photos.empty()) {
        loadPhoto(m_photos[m_photoIdx], &m_photoTex);
        m_photoPixels = textureToPixels(m_photoTex);
        kickPreloadNext();  // danh sách vừa đổi: nạp trước ảnh kế mới
    }
    m_photoTimer = 0;
}

void SaverEngine::startTransition() {
    // Load ảnh kế vào m_photoNextTex
    if (m_photoNextTex) { SDL_DestroyTexture(m_photoNextTex); m_photoNextTex = nullptr; }
    if (m_photoNextPixels) { delete[] m_photoNextPixels; m_photoNextPixels = nullptr; }
    if (!m_photos.empty()) {
        // Ưu tiên ảnh đã được thread nền giải mã trước; chưa kịp thì
        // load đồng bộ như cũ. Pixels của ảnh hiện tại đã có sẵn từ lúc
        // nó được nạp nên không tính lại ở đây.
        SDL_Texture* preTex = takePreloadedTexture(m_photos[m_photoIdx]);
        if (preTex) m_photoNextTex = preTex;
        else loadPhoto(m_photos[m_photoIdx], &m_photoNextTex);
        m_photoNextPixels = textureToPixels(m_photoNextTex);
    }
    m_transActive = true;
    m_transProgress = 0;
    // Chọn hiệu ứng: nếu m_transType == -1 (ngẫu nhiên) thì bốc random, ngược lại dùng hiệu ứng người dùng chọn
    if (m_transType == -1) {
        m_activeTransType = rand() % 24;
    } else {
        m_activeTransType = m_transType;
    }
}

void SaverEngine::photoUpdate(float dt) {
    if (m_photos.empty()) return;
    
    if (m_transActive) {
        m_transProgress += dt / TRANS_DURATION;
        if (m_transProgress >= 1.0f) {
            m_transProgress = 1.0f;
            m_transActive = false;
            // Đổi: next thành current
            if (m_photoTex) SDL_DestroyTexture(m_photoTex);
            m_photoTex = m_photoNextTex;
            m_photoNextTex = nullptr;
            if (m_photoPixels) { delete[] m_photoPixels; }
            m_photoPixels = m_photoNextPixels;
            m_photoNextPixels = nullptr;
            m_photoTimer = 0;
            kickPreloadNext();  // giải mã trước ảnh kế trong lúc chờ
            // Giữ nguyên m_transType người dùng đã cấu hình
        }
    } else {
        m_photoTimer += dt;
        if (m_photoTimer >= m_photoDuration) {
            nextPhoto();
        }
    }
}

void SaverEngine::photoRender() {
    if (!m_renderer) return;
    
    if (m_photos.empty()) {
        if (!m_settingsOpen) {
            // Chưa có ảnh: hiện hướng dẫn chuẩn phong cách RomCloud
            SDL_SetRenderDrawColor(m_renderer, 10, 14, 22, 255);
            SDL_RenderClear(m_renderer);
            
            // Vẽ hộp thông báo trung tâm
            SDL_Rect guideBox = {180, 200, 664, 280};
            SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(m_renderer, 18, 24, 38, 230);
            SDL_RenderFillRect(m_renderer, &guideBox);
            SDL_SetRenderDrawColor(m_renderer, 0, 180, 255, 255);
            SDL_RenderDrawRect(m_renderer, &guideBox);
            SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);

            drawText("CHƯA CÓ HÌNH ẢNH NÀO", 512, 230, 255, 215, 0, true, m_fontTitle);
            drawText("Vui lòng sao chép ảnh (.jpg, .png) vào thư mục:", 512, 285, 210, 225, 240, true);
            drawText("/mnt/SDCARD/Pictures/", 512, 325, 0, 210, 255, true);
            drawText("Nhấn SELECT để mở menu Cài đặt & Quản lý danh sách ảnh", 512, 385, 140, 170, 200, true);

            if (m_ui && m_footerVisible) {
                m_ui->drawAppFooter({
                    {UiTheme::PadBtn::SELECT, "Cài đặt"},
                    {UiTheme::PadBtn::DPAD, "Đổi chế độ"},
                    {UiTheme::PadBtn::B, "Thoát"}
                });
            }
            return;
        } else {
            SDL_SetRenderDrawColor(m_renderer, 10, 14, 22, 255);
            SDL_RenderClear(m_renderer);
        }
    } else {
        if (m_transActive && m_photoPixels && m_photoNextPixels) {
            // Đang transition: dùng framebuffer với 24 hiệu ứng
            applyTransition(m_framebuffer, m_photoPixels, m_photoNextPixels, m_transProgress);
            SDL_UpdateTexture(m_fbTexture, nullptr, m_framebuffer, FB_W * sizeof(uint32_t));
            SDL_RenderCopy(m_renderer, m_fbTexture, nullptr, nullptr);
        } else if (m_photoTex) {
            SDL_RenderCopy(m_renderer, m_photoTex, nullptr, nullptr);
        }
    }
    
    // P3: Settings overlay
    if (m_settingsOpen) {
        // P3-Tai: Modal danh sách chọn ảnh (Slideshow Photo Manager)
        if (m_photoListOpen) {
            // Lớp phủ mờ toàn màn hình
            SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 180);
            SDL_Rect full = {0, 0, 1024, 768};
            SDL_RenderFillRect(m_renderer, &full);

            // Khung modal danh sách
            const int modalX = 140;
            const int modalY = 85;
            const int modalW = 744;
            const int modalH = 550;
            SDL_Rect boxBg = {modalX, modalY, modalW, modalH};
            SDL_SetRenderDrawColor(m_renderer, 16, 22, 34, 245);
            SDL_RenderFillRect(m_renderer, &boxBg);

            // Viền đôi sang trọng
            SDL_SetRenderDrawColor(m_renderer, 0, 180, 255, 255);
            SDL_RenderDrawRect(m_renderer, &boxBg);
            SDL_Rect innerBorder = {modalX + 2, modalY + 2, modalW - 4, modalH - 4};
            SDL_SetRenderDrawColor(m_renderer, 25, 45, 70, 200);
            SDL_RenderDrawRect(m_renderer, &innerBorder);

            // Header modal
            drawText("DANH SÁCH HÌNH ẢNH (SLIDESHOW)", modalX + modalW / 2, modalY + 18, 0, 225, 255, true, m_fontTitle);
            SDL_SetRenderDrawColor(m_renderer, 35, 60, 90, 255);
            SDL_Rect sep = {modalX + 24, modalY + 58, modalW - 48, 2};
            SDL_RenderFillRect(m_renderer, &sep);
            SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);

            if (m_allPhotos.empty()) {
                drawText("Không tìm thấy tệp ảnh nào trong thư mục!", modalX + modalW / 2, modalY + 240, 255, 120, 120, true);
            } else {
                // Lưới thumbnail 4x3: ảnh nhỏ + ô check bật/tắt hiển thị
                pumpThumbs();
                const int COLS = 4, ROWS = 3, PAGE = COLS * ROWS;
                const int gridX = modalX + 24;
                const int gridY = modalY + 72;
                const int gapX = 12, gapY = 12;
                const int cellW = (modalW - 48 - (COLS - 1) * gapX) / COLS;
                const int cellH = 128;
                for (int i = 0; i < PAGE && (m_photoListScroll + i) < (int)m_allPhotos.size(); i++) {
                    int idx = m_photoListScroll + i;
                    int col = i % COLS, row = i / COLS;
                    int cx = gridX + col * (cellW + gapX);
                    int cy = gridY + row * (cellH + gapY);
                    bool sel = (idx == m_photoListSel);
                    bool hidden = isPhotoHidden(m_allPhotos[idx]);

                    // Ô ảnh (thumbnail giữ đúng tỷ lệ, canh giữa)
                    SDL_Rect cellRect = {cx, cy, cellW, cellH - 22};
                    SDL_SetRenderDrawColor(m_renderer, 24, 34, 48, 255);
                    SDL_RenderFillRect(m_renderer, &cellRect);
                    SDL_Texture* thumb = getThumb(m_allPhotos[idx]);
                    if (thumb) {
                        int tw = 0, th = 0;
                        SDL_QueryTexture(thumb, nullptr, nullptr, &tw, &th);
                        if (tw > 0 && th > 0) {
                            float s = std::min((float)cellRect.w / tw, (float)cellRect.h / th);
                            int dw = (int)(tw * s), dh = (int)(th * s);
                            SDL_Rect dst = {cx + (cellRect.w - dw) / 2, cy + (cellRect.h - dh) / 2, dw, dh};
                            SDL_RenderCopy(m_renderer, thumb, nullptr, &dst);
                        }
                    }
                    if (hidden) {
                        // Ảnh đã ẩn: phủ mờ lên thumbnail
                        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 120);
                        SDL_RenderFillRect(m_renderer, &cellRect);
                        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
                    }
                    if (sel) {
                        SDL_SetRenderDrawColor(m_renderer, 0, 200, 255, 255);
                        SDL_RenderDrawRect(m_renderer, &cellRect);
                        SDL_Rect cellRect2 = {cx + 1, cy + 1, cellRect.w - 2, cellRect.h - 2};
                        SDL_RenderDrawRect(m_renderer, &cellRect2);
                    } else {
                        SDL_SetRenderDrawColor(m_renderer, 40, 60, 85, 255);
                        SDL_RenderDrawRect(m_renderer, &cellRect);
                    }

                    // Ô check ở góc trái trên: xanh + ô trắng = đang hiện,
                    // xám + dấu X đỏ = đang ẩn
                    SDL_Rect chk = {cx + 6, cy + 6, 20, 20};
                    if (!hidden) {
                        SDL_SetRenderDrawColor(m_renderer, 0, 200, 120, 255);
                        SDL_RenderFillRect(m_renderer, &chk);
                        SDL_Rect inner = {cx + 11, cy + 11, 10, 10};
                        SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255);
                        SDL_RenderFillRect(m_renderer, &inner);
                    } else {
                        SDL_SetRenderDrawColor(m_renderer, 70, 75, 85, 255);
                        SDL_RenderFillRect(m_renderer, &chk);
                        SDL_SetRenderDrawColor(m_renderer, 255, 90, 90, 255);
                        SDL_RenderDrawLine(m_renderer, chk.x + 4, chk.y + 4, chk.x + chk.w - 5, chk.y + chk.h - 5);
                        SDL_RenderDrawLine(m_renderer, chk.x + chk.w - 5, chk.y + 4, chk.x + 4, chk.y + chk.h - 5);
                    }

                    // Tên tệp dưới ảnh (cắt bớt nếu dài quá ô)
                    std::string name = getPhotoName(m_allPhotos[idx]);
                    while (!name.empty() && getTextWidth(name.c_str()) > cellW) {
                        name.pop_back();
                        while (!name.empty() && ((unsigned char)name.back() & 0xC0) == 0x80) name.pop_back();
                    }
                    if (sel) drawText(name.c_str(), cx + cellW / 2, cy + cellH - 20, 255, 255, 255, true);
                    else drawText(name.c_str(), cx + cellW / 2, cy + cellH - 20, 170, 190, 210, true);
                }

                // Dòng trạng thái tổng kết ở đáy modal
                int hiddenCount = (int)m_hiddenPhotos.size();
                int shownCount = (int)m_photos.size();
                char summaryBuf[128];
                snprintf(summaryBuf, sizeof(summaryBuf), "Đang chọn %d / %d ảnh  •  %d hiển thị, %d đã ẩn",
                         m_photoListSel + 1, (int)m_allPhotos.size(), shownCount, hiddenCount);
                drawText(summaryBuf, modalX + modalW / 2, modalY + 490, 140, 165, 195, true);
            }

            if (m_ui) {
                m_ui->drawAppFooter({
                    {UiTheme::PadBtn::DPAD, "Di chuyển"},
                    {UiTheme::PadBtn::A, "Chọn / bỏ chọn"},
                    {UiTheme::PadBtn::START, "Đóng"},
                    {UiTheme::PadBtn::B, "Quay lại"}
                });
            }
            return;
        }

        // Modal Cài Đặt Trình Chiếu Ảnh (Slideshow Settings)
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 180);
        SDL_Rect full = {0, 0, 1024, 768};
        SDL_RenderFillRect(m_renderer, &full);

        // Khung modal chính
        const int modalX = 160;
        const int modalY = 120;
        const int modalW = 704;
        const int modalH = 490;
        SDL_Rect mBg = {modalX, modalY, modalW, modalH};
        SDL_SetRenderDrawColor(m_renderer, 16, 22, 34, 245);
        SDL_RenderFillRect(m_renderer, &mBg);

        // Viền cyan phong cách RomCloud
        SDL_SetRenderDrawColor(m_renderer, 0, 180, 255, 255);
        SDL_RenderDrawRect(m_renderer, &mBg);
        SDL_Rect innerBorder = {modalX + 2, modalY + 2, modalW - 4, modalH - 4};
        SDL_SetRenderDrawColor(m_renderer, 25, 45, 70, 200);
        SDL_RenderDrawRect(m_renderer, &innerBorder);

        // Tiêu đề modal
        drawText("CÀI ĐẶT TRÌNH CHIẾU ẢNH", modalX + modalW / 2, modalY + 20, 0, 225, 255, true, m_fontTitle);
        SDL_SetRenderDrawColor(m_renderer, 35, 60, 90, 255);
        SDL_Rect sep = {modalX + 24, modalY + 64, modalW - 48, 2};
        SDL_RenderFillRect(m_renderer, &sep);
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);

        // Tên 24 hiệu ứng chuyển cảnh tiếng Việt chuẩn xác
        static const char* kTransNames[] = {
            "Mờ dần (Crossfade)",
            "Mờ qua đen",
            "Mờ qua trắng",
            "Trượt sang trái",
            "Trượt sang phải",
            "Trượt lên trên",
            "Trượt xuống dưới",
            "Quét sang trái",
            "Quét sang phải",
            "Quét lên trên",
            "Quét xuống dưới",
            "Rèm mở ngang",
            "Rèm mở dọc",
            "Màn trập mở",
            "Màn trập đóng",
            "Phóng to dần",
            "Thu nhỏ dần",
            "Mở rộng khung",
            "Thu hẹp khung",
            "Vòng tròn mở",
            "Vòng tròn khép",
            "Bay lướt góc",
            "Điểm ảnh vỡ (Pixelate)",
            "Bàn cờ caro"
        };

        const char* labels[] = {
            "Thời gian mỗi ảnh",
            "Hiệu ứng chuyển cảnh",
            "Thứ tự trình chiếu",
            "Quản lý danh sách ảnh",
            "Đóng cài đặt"
        };

        const int startY = modalY + 84;
        const int rowH = 64;

        for (int i = 0; i < 5; i++) {
            int y = startY + i * rowH;
            bool sel = (i == m_settingsSel);

            // Highlight hàng đang chọn
            if (sel) {
                SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
                SDL_Rect rowHl = {modalX + 16, y, modalW - 32, 54};
                SDL_SetRenderDrawColor(m_renderer, 0, 75, 125, 230);
                SDL_RenderFillRect(m_renderer, &rowHl);
                SDL_SetRenderDrawColor(m_renderer, 0, 190, 255, 255);
                SDL_RenderDrawRect(m_renderer, &rowHl);
                SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
            }

            // Vẽ Nhãn (căn lề trái)
            uint8_t lr = sel ? 255 : 190;
            uint8_t lg = sel ? 255 : 205;
            uint8_t lb = sel ? 255 : 220;
            drawText(labels[i], modalX + 35, y + 14, lr, lg, lb);

            // Chuẩn bị chuỗi Giá trị
            char valBuf[128];
            if (i == 0) {
                // NotoSans chuẩn không có glyph ◀ ▶ (U+25C0/U+25B6) -> dùng
                // mũi tên ← → (U+2190/U+2192, đã verify có trong font).
                if (sel) snprintf(valBuf, sizeof(valBuf), "←  %.0f giây  →", m_photoDuration);
                else snprintf(valBuf, sizeof(valBuf), "%.0f giây", m_photoDuration);
            } else if (i == 1) {
                const char* tname = "Ngẫu nhiên (Tất cả)";
                if (m_transType >= 0 && m_transType < 24) {
                    tname = kTransNames[m_transType];
                }
                if (sel) snprintf(valBuf, sizeof(valBuf), "←  %s  →", tname);
                else snprintf(valBuf, sizeof(valBuf), "%s", tname);
            } else if (i == 2) {
                const char* orderStr = m_photoShuffle ? "Ngẫu nhiên (Shuffle)" : "Theo thứ tự (Tuần tự)";
                if (sel) snprintf(valBuf, sizeof(valBuf), "←  %s  →", orderStr);
                else snprintf(valBuf, sizeof(valBuf), "%s", orderStr);
            } else if (i == 3) {
                int total = (int)m_allPhotos.size();
                int shown = (int)m_photos.size();
                if (sel) snprintf(valBuf, sizeof(valBuf), "%d / %d ảnh  [Nhấn A]", shown, total);
                else snprintf(valBuf, sizeof(valBuf), "%d / %d ảnh", shown, total);
            } else {
                snprintf(valBuf, sizeof(valBuf), "Nhấn A hoặc B");
            }

            // Vẽ Giá trị (căn lề phải)
            int valW = getTextWidth(valBuf);
            int valX = modalX + modalW - 35 - valW;
            uint8_t vr = sel ? 0 : 130;
            uint8_t vg = sel ? 240 : 175;
            uint8_t vb = sel ? 160 : 205;
            if (i == 4) {
                vr = sel ? 255 : 160;
                vg = sel ? 215 : 180;
                vb = sel ? 0 : 200;
            }
            drawText(valBuf, valX, y + 14, vr, vg, vb);
        }

        if (m_ui) {
            m_ui->drawAppFooter({
                {UiTheme::PadBtn::DPAD, "Chọn mục / Đổi giá trị"},
                {UiTheme::PadBtn::A, "Thay đổi"},
                {UiTheme::PadBtn::START, "Đóng"},
                {UiTheme::PadBtn::B, "Quay lại"}
            });
        }
        return;
    }

    // Chế độ xem ảnh bình thường: Footer Hint chuẩn RomCloud
    // (START ẩn/hiện thanh footer này — Tai 2026-10-10)
    if (m_ui && m_footerVisible) {
        m_ui->drawAppFooter({
            {UiTheme::PadBtn::SELECT, "Cài đặt"},
            {UiTheme::PadBtn::START, "Ẩn footer"},
            {UiTheme::PadBtn::L1R1, "Ảnh trước / sau"},
            {UiTheme::PadBtn::A, "Ẩn ảnh này"},
            {UiTheme::PadBtn::DPAD, "Đổi chế độ"},
            {UiTheme::PadBtn::B, "Thoát"}
        });
    }
}

void SaverEngine::applyTransition(uint32_t* out, uint32_t* oldFb, uint32_t* newFb, float progress) {
    if (!out || !oldFb || !newFb) return;
    
    int type = m_activeTransType;
    if (type < 0 || type >= 24) type = 0;  // fallback crossfade
    
    float p = progress;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    
    switch (type) {
        case 0: { // Crossfade
            for (int i = 0; i < FB_W * FB_H; i++) {
                uint32_t o = oldFb[i], n = newFb[i];
                uint8_t r = (uint8_t)(((o & 0xFF) * (1-p) + (n & 0xFF) * p));
                uint8_t g = (uint8_t)((((o>>8)&0xFF) * (1-p) + ((n>>8)&0xFF) * p));
                uint8_t b = (uint8_t)((((o>>16)&0xFF) * (1-p) + ((n>>16)&0xFF) * p));
                out[i] = (0xFFu<<24) | (b<<16) | (g<<8) | r;
            }
            break;
        }
        case 1: { // Fade to black (p<0.5: fade out, p>=0.5: fade in)
            float q = (p < 0.5f) ? (p * 2) : ((1 - p) * 2);
            uint32_t* src = (p < 0.5f) ? oldFb : newFb;
            for (int i = 0; i < FB_W * FB_H; i++) {
                uint32_t s = src[i];
                uint8_t r = (uint8_t)((s & 0xFF) * q);
                uint8_t g = (uint8_t)(((s>>8)&0xFF) * q);
                uint8_t b = (uint8_t)(((s>>16)&0xFF) * q);
                out[i] = (0xFFu<<24) | (b<<16) | (g<<8) | r;
            }
            break;
        }
        case 2: { // Fade to white
            float q = (p < 0.5f) ? (p * 2) : ((1 - p) * 2);
            uint32_t* src = (p < 0.5f) ? oldFb : newFb;
            for (int i = 0; i < FB_W * FB_H; i++) {
                uint32_t s = src[i];
                uint8_t r = (uint8_t)((s & 0xFF) * q + 255 * (1-q));
                uint8_t g = (uint8_t)(((s>>8)&0xFF) * q + 255 * (1-q));
                uint8_t b = (uint8_t)(((s>>16)&0xFF) * q + 255 * (1-q));
                out[i] = (0xFFu<<24) | (b<<16) | (g<<8) | r;
            }
            break;
        }
        case 3: case 4: case 5: case 6: { // Slide (push)
            int dx = 0, dy = 0;
            if (type == 3) dx = -(int)(FB_W * p);      // new từ phải
            else if (type == 4) dx = (int)(FB_W * p);  // new từ trái
            else if (type == 5) dy = -(int)(FB_H * p); // new từ dưới
            else dy = (int)(FB_H * p);                 // new từ trên
            
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int nx = x - dx, ny = y - dy;  // vị trí trong ảnh mới
                    int ox = x - dx + (type==3?FB_W:type==4?-FB_W:0);
                    int oy = y - dy + (type==5?FB_H:type==6?-FB_H:0);
                    uint32_t col = 0xFF000000;
                    if (nx >= 0 && nx < FB_W && ny >= 0 && ny < FB_H) {
                        col = newFb[ny * FB_W + nx];
                    } else if (ox >= 0 && ox < FB_W && oy >= 0 && oy < FB_H) {
                        col = oldFb[oy * FB_W + ox];
                    }
                    out[y * FB_W + x] = col;
                }
            }
            break;
        }
        case 7: case 8: case 9: case 10: { // Wipe (reveal)
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    bool useNew = false;
                    if (type == 7) useNew = (x < FB_W * p);       // từ trái
                    else if (type == 8) useNew = (x > FB_W * (1-p)); // từ phải
                    else if (type == 9) useNew = (y < FB_H * p);  // từ trên
                    else useNew = (y > FB_H * (1-p));             // từ dưới
                    out[y * FB_W + x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 11: { // Blinds horizontal (8 sọc)
            int strips = 8;
            int sh = FB_H / strips;
            for (int y = 0; y < FB_H; y++) {
                int strip = y / sh;
                // Mỗi sọc wipe với offset khác nhau
                float lp = p * 1.2f - (strip % 2) * 0.2f;
                if (lp < 0) lp = 0; if (lp > 1) lp = 1;
                bool useNew = (y % sh) < (sh * lp);
                for (int x = 0; x < FB_W; x++) {
                    out[y*FB_W+x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 12: { // Blinds vertical (8 sọc)
            int strips = 8;
            int sw = FB_W / strips;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int strip = x / sw;
                    float lp = p * 1.2f - (strip % 2) * 0.2f;
                    if (lp < 0) lp = 0; if (lp > 1) lp = 1;
                    bool useNew = (x % sw) < (sw * lp);
                    out[y*FB_W+x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 13: { // Shutter open (mở từ giữa ra 2 bên)
            int mid = FB_W / 2;
            int openW = (int)(mid * p);
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    bool useNew = (x > mid - openW && x < mid + openW);
                    out[y*FB_W+x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 14: { // Shutter close (đóng từ 2 bên vào giữa, rồi mở ảnh mới)
            // Đơn giản: wipe từ 2 biên vào giữa
            int w = (int)(FB_W * p / 2);
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    bool useNew = (x < w || x > FB_W - w);
                    out[y*FB_W+x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 15: { // Zoom in (ảnh mới phóng to từ giữa)
            float scale = 0.5f + p * 0.5f;  // 0.5 -> 1.0
            int cx = FB_W/2, cy = FB_H/2;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int sx = cx + (int)((x - cx) / scale);
                    int sy = cy + (int)((y - cy) / scale);
                    uint32_t col = 0xFF000000;
                    if (sx >= 0 && sx < FB_W && sy >= 0 && sy < FB_H) {
                        col = newFb[sy * FB_W + sx];
                    }
                    // Blend với ảnh cũ ở rìa
                    if (p < 1.0f && (sx < 0 || sx >= FB_W || sy < 0 || sy >= FB_H)) {
                        col = oldFb[y*FB_W+x];
                    }
                    out[y*FB_W+x] = col;
                }
            }
            break;
        }
        case 16: { // Zoom out (ảnh cũ thu nhỏ, lộ ảnh mới)
            float scale = 1.0f - p * 0.9f;  // 1.0 -> 0.1
            int cx = FB_W/2, cy = FB_H/2;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int sx = cx + (int)((x - cx) / scale);
                    int sy = cy + (int)((y - cy) / scale);
                    uint32_t col = newFb[y*FB_W+x];  // nền là ảnh mới
                    if (sx >= 0 && sx < FB_W && sy >= 0 && sy < FB_H) {
                        col = oldFb[sy * FB_W + sx];  // ảnh cũ thu nhỏ ở giữa
                    }
                    out[y*FB_W+x] = col;
                }
            }
            break;
        }
        case 17: { // Box in (hộp vuông phóng to)
            int size = (int)(50 + p * (FB_W - 50));
            int x0 = (FB_W - size)/2, y0 = (FB_H - size)/2;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    bool inside = (x >= x0 && x < x0+size && y >= y0 && y < y0+size);
                    out[y*FB_W+x] = inside ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 18: { // Box out (hộp vuông thu nhỏ)
            int size = (int)(FB_W - p * (FB_W - 50));
            int x0 = (FB_W - size)/2, y0 = (FB_H - size)/2;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    bool inside = (x >= x0 && x < x0+size && y >= y0 && y < y0+size);
                    out[y*FB_W+x] = inside ? oldFb[y*FB_W+x] : newFb[y*FB_W+x];
                }
            }
            break;
        }
        case 19: { // Fly away (ảnh cũ bay về góc trên-phải, nhỏ dần)
            float scale = 1.0f - p * 0.8f;
            int ox = (int)(p * FB_W * 0.5f);   // bay sang phải
            int oy = (int)(-p * FB_H * 0.5f);  // bay lên trên
            int cx = FB_W/2, cy = FB_H/2;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    uint32_t col = newFb[y*FB_W+x];
                    int sx = cx + (int)((x - cx - ox) / scale);
                    int sy = cy + (int)((y - cy - oy) / scale);
                    if (sx >= 0 && sx < FB_W && sy >= 0 && sy < FB_H) {
                        // Blend: ảnh cũ mờ dần khi bay
                        uint32_t o = oldFb[sy*FB_W+sx];
                        uint32_t n = newFb[y*FB_W+x];
                        float a = 1 - p;
                        uint8_t r = (uint8_t)(((o&0xFF)*a + (n&0xFF)*(1-a)));
                        uint8_t g = (uint8_t)((((o>>8)&0xFF)*a + ((n>>8)&0xFF)*(1-a)));
                        uint8_t b = (uint8_t)((((o>>16)&0xFF)*a + ((n>>16)&0xFF)*(1-a)));
                        col = (0xFFu<<24)|(b<<16)|(g<<8)|r;
                    }
                    out[y*FB_W+x] = col;
                }
            }
            break;
        }
        case 20: { // Iris open (tròn mở từ giữa)
            int cx = FB_W/2, cy = FB_H/2;
            int maxR = (int)sqrt(cx*cx + cy*cy);
            int r = (int)(maxR * p);
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int dx = x - cx, dy = y - cy;
                    bool inside = (dx*dx + dy*dy) <= r*r;
                    out[y*FB_W+x] = inside ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        case 21: { // Iris close (tròn đóng vào giữa)
            int cx = FB_W/2, cy = FB_H/2;
            int maxR = (int)sqrt(cx*cx + cy*cy);
            int r = (int)(maxR * (1-p));
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int dx = x - cx, dy = y - cy;
                    bool inside = (dx*dx + dy*dy) <= r*r;
                    out[y*FB_W+x] = inside ? oldFb[y*FB_W+x] : newFb[y*FB_W+x];
                }
            }
            break;
        }
        case 22: { // Pixelate (ô vuông to dần nhỏ)
            int block = (int)(40 * (1-p) + 1);
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int bx = (x / block) * block;
                    int by = (y / block) * block;
                    // Dùng ảnh mới nếu block đã "hiện", ngược lại ảnh cũ
                    // Đơn giản: blend theo p với block
                    float bp = p;
                    uint32_t o = oldFb[y*FB_W+x];
                    // Lấy màu đại diện của block từ ảnh mới
                    int sx = bx + block/2, sy = by + block/2;
                    if (sx >= FB_W) sx = FB_W-1;
                    if (sy >= FB_H) sy = FB_H-1;
                    uint32_t n = newFb[sy*FB_W+sx];
                    uint8_t r = (uint8_t)(((o&0xFF)*(1-bp) + (n&0xFF)*bp));
                    uint8_t g = (uint8_t)((((o>>8)&0xFF)*(1-bp) + ((n>>8)&0xFF)*bp));
                    uint8_t b = (uint8_t)((((o>>16)&0xFF)*(1-bp) + ((n>>16)&0xFF)*bp));
                    out[y*FB_W+x] = (0xFFu<<24)|(b<<16)|(g<<8)|r;
                }
            }
            break;
        }
        case 23: { // Checkerboard (ô cờ)
            int cell = 60;
            for (int y = 0; y < FB_H; y++) {
                for (int x = 0; x < FB_W; x++) {
                    int cx = x / cell, cy = y / cell;
                    bool even = ((cx + cy) % 2) == 0;
                    // Ô chẵn hiện trước, ô lẻ hiện sau
                    float threshold = even ? 0.5f : 1.0f;
                    bool useNew = p > (even ? p*0.5f : (p-0.5f)*2);
                    if (p < 0.5f) useNew = even ? (p*2 > 0.5f) : false;
                    else useNew = even ? true : ((p-0.5f)*2 > 0.5f);
                    out[y*FB_W+x] = useNew ? newFb[y*FB_W+x] : oldFb[y*FB_W+x];
                }
            }
            break;
        }
        default: { // Fallback crossfade
            for (int i = 0; i < FB_W * FB_H; i++) {
                uint32_t o = oldFb[i], n = newFb[i];
                uint8_t r = (uint8_t)(((o & 0xFF) * (1-p) + (n & 0xFF) * p));
                uint8_t g = (uint8_t)((((o>>8)&0xFF) * (1-p) + ((n>>8)&0xFF) * p));
                uint8_t b = (uint8_t)((((o>>16)&0xFF) * (1-p) + ((n>>16)&0xFF) * p));
                out[i] = (0xFFu<<24) | (b<<16) | (g<<8) | r;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// P3: Settings menu
// ---------------------------------------------------------------------------

int SaverEngine::getTextWidth(const char* text, TTF_Font* customFont) {
    TTF_Font* font = customFont ? customFont : m_font;
    if (!font || !text || !text[0]) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    return w;
}

void SaverEngine::drawText(const char* text, int x, int y, uint8_t r, uint8_t g, uint8_t b, bool centered, TTF_Font* customFont) {
    TTF_Font* font = customFont ? customFont : m_font;
    if (!font || !m_renderer || !text || !text[0]) return;
    SDL_Color col = {r, g, b, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    int w = 0, h = 0;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    int drawX = centered ? (x - w / 2) : x;
    SDL_Rect dst = {drawX, y, w, h};
    SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void SaverEngine::toggleSettings() {
    m_settingsOpen = !m_settingsOpen;
    m_settingsSel = 0;
}

void SaverEngine::settingsInput(int button) {
    // Button enum: UP=0, DOWN=1, LEFT=2, RIGHT=3, A=4, B=5, START=12
    const int BTN_UP = 0;
    const int BTN_DOWN = 1;
    const int BTN_LEFT = 2;
    const int BTN_RIGHT = 3;
    const int BTN_A = 4;
    const int BTN_B = 5;
    const int BTN_START = 12;
    
    // START: Đóng settings hoặc đóng danh sách ảnh
    if (button == BTN_START) {
        if (m_photoListOpen) {
            m_photoListOpen = false;
        } else {
            m_settingsOpen = false;
        }
        return;
    }
    
    // P3-Tai: đang mở danh sách ảnh
    if (m_photoListOpen) {
        if (button == BTN_B) {
            m_photoListOpen = false;
            return;
        }
        if (m_allPhotos.empty()) return;
        const int GRID_COLS = 4, GRID_PAGE = 12;
        int nPhotos = (int)m_allPhotos.size();
        if (button == BTN_UP) {
            m_photoListSel = std::max(0, m_photoListSel - GRID_COLS);
        } else if (button == BTN_DOWN) {
            m_photoListSel = std::min(nPhotos - 1, m_photoListSel + GRID_COLS);
        } else if (button == BTN_LEFT) {
            m_photoListSel = std::max(0, m_photoListSel - 1);
        } else if (button == BTN_RIGHT) {
            m_photoListSel = std::min(nPhotos - 1, m_photoListSel + 1);
        } else if (button == BTN_A) {
            // A: bật/tắt hiển thị ảnh đang chọn. Cập nhật tại chỗ, không
            // quét lại danh sách để thứ tự trong lưới không bị xáo trộn.
            if (m_photoListSel >= 0 && m_photoListSel < nPhotos) {
                std::string p = m_allPhotos[m_photoListSel];
                auto it = std::find(m_hiddenPhotos.begin(), m_hiddenPhotos.end(), p);
                if (it != m_hiddenPhotos.end()) {
                    m_hiddenPhotos.erase(it);  // hiện lại
                    m_photos.push_back(p);
                } else {
                    m_hiddenPhotos.push_back(p);  // ẩn
                    auto ip = std::find(m_photos.begin(), m_photos.end(), p);
                    if (ip != m_photos.end()) m_photos.erase(ip);
                }
            }
        }
        // Giữ ô đang chọn nằm trong trang lưới đang xem
        m_photoListScroll = (m_photoListSel / GRID_PAGE) * GRID_PAGE;
        return;
    }
    
    if (button == BTN_B) {
        m_settingsOpen = false;
        return;
    }
    if (button == BTN_UP) {
        m_settingsSel = (m_settingsSel - 1 + 5) % 5;
    } else if (button == BTN_DOWN) {
        m_settingsSel = (m_settingsSel + 1) % 5;
    } else if (button == BTN_A) {
        if (m_settingsSel == 0) {
            float vals[] = {3, 5, 10, 15, 30, 60};
            int idx = 0;
            for (int i = 0; i < 6; i++) if (vals[i] == m_photoDuration) { idx = i; break; }
            idx = (idx + 1) % 6;
            m_photoDuration = vals[idx];
        } else if (m_settingsSel == 1) {
            m_transType++;
            if (m_transType > 23) m_transType = -1;
        } else if (m_settingsSel == 2) {
            m_photoShuffle = !m_photoShuffle;
            scanPhotos();
        } else if (m_settingsSel == 3) {
            // Mở danh sách chọn ảnh
            m_photoListOpen = true;
            m_photoListSel = 0;
            m_photoListScroll = 0;
        } else if (m_settingsSel == 4) {
            // Đóng cài đặt
            m_settingsOpen = false;
        }
        return;
    } else if (button == BTN_LEFT || button == BTN_RIGHT) {
        int dir = (button == BTN_RIGHT) ? 1 : -1;
        if (m_settingsSel == 0) {
            float vals[] = {3, 5, 10, 15, 30, 60};
            int idx = 0;
            for (int i = 0; i < 6; i++) if (vals[i] == m_photoDuration) { idx = i; break; }
            idx = (idx + dir + 6) % 6;
            m_photoDuration = vals[idx];
        } else if (m_settingsSel == 1) {
            m_transType += dir;
            if (m_transType < -1) m_transType = 23;
            if (m_transType > 23) m_transType = -1;
        } else if (m_settingsSel == 2) {
            m_photoShuffle = !m_photoShuffle;
            scanPhotos();
        }
    }
}

const char* SaverEngine::getPhotoName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) return path.c_str() + pos + 1;
    return path.c_str();
}

bool SaverEngine::isPhotoHidden(const std::string& path) {
    return std::find(m_hiddenPhotos.begin(), m_hiddenPhotos.end(), path) != m_hiddenPhotos.end();
}

} // namespace RomCloud

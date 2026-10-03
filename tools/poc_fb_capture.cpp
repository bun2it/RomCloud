#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <chrono>
#include <vector>
#include <fstream>
#include <cstring>

int main() {
    std::cout << "=== TrimUI Brick Pro Framebuffer Capture Benchmark ===" << std::endl;
    int fb_fd = open("/dev/fb0", O_RDONLY);
    if (fb_fd < 0) {
        std::cerr << "Failed to open /dev/fb0" << std::endl;
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        std::cerr << "Error reading variable information" << std::endl;
        close(fb_fd);
        return 1;
    }

    std::cout << "Resolution: " << vinfo.xres << "x" << vinfo.yres 
              << " (" << vinfo.bits_per_pixel << " bpp)" << std::endl;
    
    long screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
    std::cout << "Frame size: " << screensize << " bytes (" 
              << (screensize / (1024.0 * 1024.0)) << " MB)" << std::endl;

    uint8_t* fbp = (uint8_t*)mmap(0, screensize, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fbp == MAP_FAILED) {
        std::cerr << "Failed to mmap framebuffer" << std::endl;
        close(fb_fd);
        return 1;
    }

    // Benchmark 1: Zero-copy read speed
    const int NUM_FRAMES = 120;
    std::vector<uint8_t> buffer(screensize);
    std::cout << "\nBenchmarking 1:1 Memory Copy (" << NUM_FRAMES << " frames)..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_FRAMES; ++i) {
        std::memcpy(buffer.data(), fbp, screensize);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;

    double avgMsPerFrame = elapsed.count() / NUM_FRAMES;
    double fps = 1000.0 / avgMsPerFrame;
    double mbPerSec = (screensize * NUM_FRAMES) / (elapsed.count() / 1000.0) / (1024.0 * 1024.0);

    std::cout << "-> Time per frame: " << avgMsPerFrame << " ms" << std::endl;
    std::cout << "-> Theoretical Max FPS: " << fps << " FPS" << std::endl;
    std::cout << "-> Memory Throughput: " << mbPerSec << " MB/s" << std::endl;

    // Benchmark 2: Downsampled 2x (512x384 RGB)
    int dw = vinfo.xres / 2;
    int dh = vinfo.yres / 2;
    std::vector<uint8_t> downsampled(dw * dh * 3);
    std::cout << "\nBenchmarking 2x Downscale (1024x768 BGRA -> 512x384 RGB)..." << std::endl;

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_FRAMES; ++i) {
        const uint32_t* src = (const uint32_t*)fbp;
        uint8_t* dst = downsampled.data();
        for (int y = 0; y < dh; ++y) {
            const uint32_t* row = src + (y * 2) * vinfo.xres;
            for (int x = 0; x < dw; ++x) {
                uint32_t pixel = row[x * 2];
                // BGRA to RGB
                *dst++ = (pixel >> 16) & 0xFF; // R
                *dst++ = (pixel >> 8) & 0xFF;  // G
                *dst++ = pixel & 0xFF;         // B
            }
        }
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;

    avgMsPerFrame = elapsed.count() / NUM_FRAMES;
    fps = 1000.0 / avgMsPerFrame;

    std::cout << "-> Time per frame: " << avgMsPerFrame << " ms" << std::endl;
    std::cout << "-> Downscaled Max FPS: " << fps << " FPS" << std::endl;

    // Save a sample frame as PPM to verify image content
    std::cout << "\nSaving sample frame to /tmp/screenshot.ppm..." << std::endl;
    std::ofstream ppm("/tmp/screenshot.ppm", std::ios::binary);
    if (ppm) {
        ppm << "P6\n" << dw << " " << dh << "\n255\n";
        ppm.write((char*)downsampled.data(), downsampled.size());
        ppm.close();
        std::cout << "Saved /tmp/screenshot.ppm successfully!" << std::endl;
    }

    munmap(fbp, screensize);
    close(fb_fd);
    std::cout << "Benchmark complete." << std::endl;
    return 0;
}

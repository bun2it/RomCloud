#pragma once
#include <SDL2/SDL.h>
#include <cstdint>

namespace RomCloud {

enum class Button {
    UP,
    DOWN,
    LEFT,
    RIGHT,
    A,
    B,
    X,
    Y,
    L1,
    R1,
    L2,
    R2,
    START,
    SELECT,
    MENU,
    PLAY,
    FN_LEFT,
    FN_RIGHT,
    COUNT
};

class InputManager {
public:
    static InputManager& instance();
    bool init();
    void shutdown();
    void update();

    bool isButtonPressed(Button btn) const;
    bool isButtonJustPressed(Button btn) const;
    // Typematic repeat for keyboard typing (A/X/Y): first press immediate,
    // then repeat after 400ms every 70ms while held. Use ONLY in virtual
    // keyboard input handlers — never for confirm/cancel/play actions.
    bool isButtonRepeat(Button btn) const;
    void reset();

    // Check FN buttons (F1 / F2 key, joy button, or Linux evdev)
    bool isFnLeftTriggered();
    bool isFnRightTriggered();

    // Raw analog stick positions (0..255, 128 = center)
    uint8_t getStickLX() const { return m_stickLX; }
    uint8_t getStickLY() const { return m_stickLY; }
    uint8_t getStickRX() const { return m_stickRX; }
    uint8_t getStickRY() const { return m_stickRY; }

    // Pure D-pad inputs (physical D-pad buttons, hats, keyboard arrows - excluding analog sticks)
    bool isPureDpadUp() const { return m_dpadUp || m_hatUp || m_keyUp; }
    bool isPureDpadDown() const { return m_dpadDown || m_hatDown || m_keyDown; }
    bool isPureDpadLeft() const { return m_dpadLeft || m_hatLeft || m_keyLeft; }
    bool isPureDpadRight() const { return m_dpadRight || m_hatRight || m_keyRight; }

private:
    InputManager() = default;
    SDL_GameController* m_controller = nullptr;
    SDL_Joystick* m_joystick = nullptr;

    bool m_currentStates[static_cast<int>(Button::COUNT)] = {false};
    bool m_previousStates[static_cast<int>(Button::COUNT)] = {false};

    // Directional component states (D-pad, Joystick, Keyboard)
    bool m_dpadUp = false;
    bool m_dpadDown = false;
    bool m_dpadLeft = false;
    bool m_dpadRight = false;

    bool m_axisUp = false;
    bool m_axisDown = false;
    bool m_axisLeft = false;
    bool m_axisRight = false;

    bool m_joyAxisUp = false;
    bool m_joyAxisDown = false;
    bool m_joyAxisLeft = false;
    bool m_joyAxisRight = false;

    bool m_hatUp = false;
    bool m_hatDown = false;
    bool m_hatLeft = false;
    bool m_hatRight = false;

    bool m_keyUp = false;
    bool m_keyDown = false;
    bool m_keyLeft = false;
    bool m_keyRight = false;

    bool m_axisL2 = false;
    bool m_axisR2 = false;

    // Edge latch: bắt phím nhấn-nhả nhanh kẹt giữa 2 vòng poll (IPTV loop delay 35ms + OSD nặng)
    bool m_justPressed[static_cast<int>(Button::COUNT)] = {false};

    // Hold-to-repeat timing
    uint32_t m_pressStartTime[static_cast<int>(Button::COUNT)] = {0};
    uint32_t m_lastRepeatTime[static_cast<int>(Button::COUNT)] = {0};
    // Analog stick states (0..255, 128 = center)
    uint8_t m_stickLX = 128;
    uint8_t m_stickLY = 128;
    uint8_t m_stickRX = 128;
    uint8_t m_stickRY = 128;

    void mapKeyboardKey(SDL_Keycode key, bool isDown);
};

} // namespace RomCloud

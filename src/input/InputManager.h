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
    bool isButtonJustReleased(Button btn) const;
    void reset();
    const char* getControllerName() const;

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

    // Hold-to-repeat timing
    uint32_t m_pressStartTime[static_cast<int>(Button::COUNT)] = {0};
    uint32_t m_lastRepeatTime[static_cast<int>(Button::COUNT)] = {0};

    void mapKeyboardKey(SDL_Keycode key, bool isDown);
};

} // namespace RomCloud

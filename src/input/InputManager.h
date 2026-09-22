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
    const char* getControllerName() const;

private:
    InputManager() = default;
    SDL_GameController* m_controller = nullptr;
    SDL_Joystick* m_joystick = nullptr;

    bool m_currentStates[static_cast<int>(Button::COUNT)] = {false};
    bool m_previousStates[static_cast<int>(Button::COUNT)] = {false};

    void mapKeyboardKey(SDL_Keycode key, bool isDown);
};

} // namespace RomCloud

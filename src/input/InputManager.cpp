#include "InputManager.h"
#include "../logging/Logger.h"

namespace RomCloud {

InputManager& InputManager::instance() {
    static InputManager instance;
    return instance;
}

bool InputManager::init() {
    int numJoysticks = SDL_NumJoysticks();
    Logger::info("Detected " + std::to_string(numJoysticks) + " joystick/gamepad device(s)");

    for (int i = 0; i < numJoysticks; ++i) {
        if (SDL_IsGameController(i)) {
            m_controller = SDL_GameControllerOpen(i);
            if (m_controller) {
                Logger::info(std::string("Opened GameController: ") + SDL_GameControllerName(m_controller));
                break;
            }
        }
    }

    if (!m_controller && numJoysticks > 0) {
        m_joystick = SDL_JoystickOpen(0);
        if (m_joystick) {
            Logger::info(std::string("Opened Joystick: ") + SDL_JoystickName(m_joystick));
        }
    }
    return true;
}

void InputManager::shutdown() {
    if (m_controller) {
        SDL_GameControllerClose(m_controller);
        m_controller = nullptr;
    }
    if (m_joystick) {
        SDL_JoystickClose(m_joystick);
        m_joystick = nullptr;
    }
}

const char* InputManager::getControllerName() const {
    if (m_controller) return SDL_GameControllerName(m_controller);
    if (m_joystick) return SDL_JoystickName(m_joystick);
    return "None (Keyboard)";
}

void InputManager::mapKeyboardKey(SDL_Keycode key, bool isDown) {
    Button btn = Button::COUNT;
    switch (key) {
        case SDLK_UP:     btn = Button::UP; break;
        case SDLK_DOWN:   btn = Button::DOWN; break;
        case SDLK_LEFT:   btn = Button::LEFT; break;
        case SDLK_RIGHT:  btn = Button::RIGHT; break;
        case SDLK_RETURN:
        case SDLK_a:
        case SDLK_SPACE:  btn = Button::A; break;
        case SDLK_ESCAPE:
        case SDLK_b:
        case SDLK_BACKSPACE: btn = Button::B; break;
        case SDLK_x:      btn = Button::X; break;
        case SDLK_y:      btn = Button::Y; break;
        case SDLK_q:
        case SDLK_PAGEUP: btn = Button::L1; break;
        case SDLK_e:
        case SDLK_PAGEDOWN: btn = Button::R1; break;
        case SDLK_TAB:    btn = Button::SELECT; break;
        case SDLK_F1:     btn = Button::START; break;
        case SDLK_HOME:   btn = Button::MENU; break;
        default: break;
    }
    if (btn != Button::COUNT) {
        m_currentStates[static_cast<int>(btn)] = isDown;
    }
}

void InputManager::update() {
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        m_previousStates[i] = m_currentStates[i];
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                m_currentStates[static_cast<int>(Button::MENU)] = true;
                break;

            case SDL_KEYDOWN:
                mapKeyboardKey(event.key.keysym.sym, true);
                break;

            case SDL_KEYUP:
                mapKeyboardKey(event.key.keysym.sym, false);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    m_currentStates[static_cast<int>(Button::UP)] = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  m_currentStates[static_cast<int>(Button::DOWN)] = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  m_currentStates[static_cast<int>(Button::LEFT)] = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: m_currentStates[static_cast<int>(Button::RIGHT)] = down; break;
                    // TrimUI physical Nintendo layout (Right=A, Bottom=B, Top=X, Left=Y)
                    case SDL_CONTROLLER_BUTTON_A:          m_currentStates[static_cast<int>(Button::B)] = down; break; // Bottom is physical B
                    case SDL_CONTROLLER_BUTTON_B:          m_currentStates[static_cast<int>(Button::A)] = down; break; // Right is physical A
                    case SDL_CONTROLLER_BUTTON_X:          m_currentStates[static_cast<int>(Button::Y)] = down; break; // Left is physical Y
                    case SDL_CONTROLLER_BUTTON_Y:          m_currentStates[static_cast<int>(Button::X)] = down; break; // Top is physical X
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  m_currentStates[static_cast<int>(Button::L1)] = down; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: m_currentStates[static_cast<int>(Button::R1)] = down; break;
                    case SDL_CONTROLLER_BUTTON_START:      m_currentStates[static_cast<int>(Button::START)] = down; break;
                    case SDL_CONTROLLER_BUTTON_BACK:       m_currentStates[static_cast<int>(Button::SELECT)] = down; break;
                    case SDL_CONTROLLER_BUTTON_GUIDE:      m_currentStates[static_cast<int>(Button::MENU)] = down; break;
                    default: break;
                }
                break;
            }

            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP: {
                if (!m_controller) {
                    bool down = (event.type == SDL_JOYBUTTONDOWN);
                    switch (event.jbutton.button) {
                        case 0: m_currentStates[static_cast<int>(Button::B)] = down; break; // Bottom B
                        case 1: m_currentStates[static_cast<int>(Button::A)] = down; break; // Right A
                        case 2: m_currentStates[static_cast<int>(Button::Y)] = down; break; // Left Y
                        case 3: m_currentStates[static_cast<int>(Button::X)] = down; break; // Top X
                        case 4: m_currentStates[static_cast<int>(Button::L1)] = down; break;
                        case 5: m_currentStates[static_cast<int>(Button::R1)] = down; break;
                        case 6: m_currentStates[static_cast<int>(Button::SELECT)] = down; break;
                        case 7: m_currentStates[static_cast<int>(Button::START)] = down; break;
                        case 8: m_currentStates[static_cast<int>(Button::MENU)] = down; break;
                        default: break;
                    }
                }
                break;
            }

            case SDL_JOYHATMOTION: {
                if (!m_controller) {
                    m_currentStates[static_cast<int>(Button::UP)] = (event.jhat.value & SDL_HAT_UP);
                    m_currentStates[static_cast<int>(Button::DOWN)] = (event.jhat.value & SDL_HAT_DOWN);
                    m_currentStates[static_cast<int>(Button::LEFT)] = (event.jhat.value & SDL_HAT_LEFT);
                    m_currentStates[static_cast<int>(Button::RIGHT)] = (event.jhat.value & SDL_HAT_RIGHT);
                }
                break;
            }

            case SDL_JOYAXISMOTION: {
                if (!m_controller) {
                    const int DEADZONE = 16000;
                    if (event.jaxis.axis == 0) {
                        m_currentStates[static_cast<int>(Button::LEFT)] = (event.jaxis.value < -DEADZONE);
                        m_currentStates[static_cast<int>(Button::RIGHT)] = (event.jaxis.value > DEADZONE);
                    } else if (event.jaxis.axis == 1) {
                        m_currentStates[static_cast<int>(Button::UP)] = (event.jaxis.value < -DEADZONE);
                        m_currentStates[static_cast<int>(Button::DOWN)] = (event.jaxis.value > DEADZONE);
                    }
                }
                break;
            }

            default: break;
        }
    }
}

bool InputManager::isButtonPressed(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;
    return m_currentStates[idx];
}

bool InputManager::isButtonJustPressed(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;
    return m_currentStates[idx] && !m_previousStates[idx];
}

bool InputManager::isButtonJustReleased(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;
    return !m_currentStates[idx] && m_previousStates[idx];
}

} // namespace RomCloud

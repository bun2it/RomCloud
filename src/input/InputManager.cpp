#include "InputManager.h"
#include "../logging/Logger.h"
#include "../platform/PlatformInfo.h"
#include "../brickpad/BrickPadManager.h"
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/input.h>
#endif

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

void InputManager::reset() {
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        m_currentStates[i] = false;
        m_previousStates[i] = false;
        m_justPressed[i] = false;
        m_pressStartTime[i] = 0;
        m_lastRepeatTime[i] = 0;
    }
    m_dpadUp = m_dpadDown = m_dpadLeft = m_dpadRight = false;
    m_axisUp = m_axisDown = m_axisLeft = m_axisRight = false;
    m_joyAxisUp = m_joyAxisDown = m_joyAxisLeft = m_joyAxisRight = false;
    m_hatUp = m_hatDown = m_hatLeft = m_hatRight = false;
    m_keyUp = m_keyDown = m_keyLeft = m_keyRight = false;
    m_axisL2 = m_axisR2 = false;
    m_stickLX = m_stickLY = m_stickRX = m_stickRY = 128;
}

void InputManager::mapKeyboardKey(SDL_Keycode key, bool isDown) {
    Button btn = Button::COUNT;
    switch (key) {
        case SDLK_UP:     m_keyUp = isDown; break;
        case SDLK_DOWN:   m_keyDown = isDown; break;
        case SDLK_LEFT:   m_keyLeft = isDown; break;
        case SDLK_RIGHT:  m_keyRight = isDown; break;
        case SDLK_RETURN:
        case SDLK_a:
        case SDLK_SPACE:  btn = Button::A; break;
        case SDLK_ESCAPE:
        case SDLK_b:
        case SDLK_BACKSPACE: btn = Button::B; break;
        case SDLK_x:      btn = Button::X; break;
        case SDLK_y:      btn = Button::Y; break;
        case SDLK_q:
        case SDLK_LSHIFT:
        case SDLK_PAGEUP: btn = Button::L1; break;
        case SDLK_e:
        case SDLK_RSHIFT:
        case SDLK_PAGEDOWN: btn = Button::R1; break;
        case SDLK_1:
        case SDLK_z:      btn = Button::L2; break;
        case SDLK_2:
        case SDLK_c:      btn = Button::R2; break;
        case SDLK_TAB:
        case SDLK_PERIOD: btn = Button::SELECT; break;
        case SDLK_F1:     btn = Button::FN_LEFT; break;
        case SDLK_F2:     btn = Button::FN_RIGHT; break;
        case SDLK_SLASH:  btn = Button::START; break;
        case SDLK_HOME:
        case SDLK_COMMA:  btn = Button::MENU; break;
        case SDLK_p:      btn = Button::PLAY; break;
        default: break;
    }
    if (btn != Button::COUNT) {
        int bi = static_cast<int>(btn);
        if (isDown && !m_currentStates[bi]) m_justPressed[bi] = true;
        m_currentStates[bi] = isDown;
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

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    PlatformInfo::instance().setDisplayMetrics(event.window.data1, event.window.data2);
                }
                break;

            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_F1 || event.key.keysym.scancode == SDL_SCANCODE_F1) {
                    int bi = static_cast<int>(Button::FN_LEFT);
                    if (!m_currentStates[bi]) m_justPressed[bi] = true;
                    m_currentStates[bi] = true;
                } else if (event.key.keysym.sym == SDLK_F2 || event.key.keysym.scancode == SDL_SCANCODE_F2) {
                    int bi = static_cast<int>(Button::FN_RIGHT);
                    if (!m_currentStates[bi]) m_justPressed[bi] = true;
                    m_currentStates[bi] = true;
                }
                mapKeyboardKey(event.key.keysym.sym, true);
                break;

            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_F1 || event.key.keysym.scancode == SDL_SCANCODE_F1) {
                    int bi = static_cast<int>(Button::FN_LEFT);
                    m_currentStates[bi] = false;
                } else if (event.key.keysym.sym == SDLK_F2 || event.key.keysym.scancode == SDL_SCANCODE_F2) {
                    int bi = static_cast<int>(Button::FN_RIGHT);
                    m_currentStates[bi] = false;
                }
                mapKeyboardKey(event.key.keysym.sym, false);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
                // Latch ngay khi thấy sự kiện DOWN: bấm-nhả nhanh trong cùng 1 vòng
                // poll (DOWN+UP gộp lại -> current cuối vẫn false) vẫn không mất nút.
                auto setBtn = [&](Button b, bool d) {
                    int bi = static_cast<int>(b);
                    if (d && !m_currentStates[bi]) m_justPressed[bi] = true;
                    m_currentStates[bi] = d;
                };
                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    m_dpadUp = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  m_dpadDown = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  m_dpadLeft = down; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: m_dpadRight = down; break;
                    // TrimUI physical Nintendo layout (Right=A, Bottom=B, Top=X, Left=Y)
                    case SDL_CONTROLLER_BUTTON_A:          setBtn(Button::B, down); break; // Bottom is physical B
                    case SDL_CONTROLLER_BUTTON_B:          setBtn(Button::A, down); break; // Right is physical A
                    case SDL_CONTROLLER_BUTTON_X:          setBtn(Button::Y, down); break; // Left is physical Y
                    case SDL_CONTROLLER_BUTTON_Y:          setBtn(Button::X, down); break; // Top is physical X
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  setBtn(Button::L1, down); break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: setBtn(Button::R1, down); break;
                    case SDL_CONTROLLER_BUTTON_START:      setBtn(Button::START, down); break;
                    // Brick chin buttons (order L to R: MENU - SELECT - START)
                    // nút trái = GUIDE (MENU), nút giữa = BACK (SELECT), nút phải = START
                    case SDL_CONTROLLER_BUTTON_BACK:       setBtn(Button::SELECT, down); break;
                    case SDL_CONTROLLER_BUTTON_GUIDE:      setBtn(Button::MENU, down); break;
                    case SDL_CONTROLLER_BUTTON_MISC1:
                    case SDL_CONTROLLER_BUTTON_PADDLE1:
                        setBtn(Button::FN_LEFT, down); break;
                    case SDL_CONTROLLER_BUTTON_PADDLE2:
                        setBtn(Button::FN_RIGHT, down); break;
                    case SDL_CONTROLLER_BUTTON_TOUCHPAD:
                        setBtn(Button::PLAY, down); break;
                    default: break;
                }
                break;
            }

            case SDL_CONTROLLERAXISMOTION: {
                // Analog sticks on TrimUI Brick Pro / Brick Pro S
                const int DEADZONE = 12000;
                const int RELEASE_ZONE = 9000;
                int axis = event.caxis.axis;
                int16_t val = event.caxis.value;

                auto toByte = [](int16_t v) -> uint8_t {
                    int mapped = ((int)v + 32768) / 256;
                    if (mapped < 0) mapped = 0;
                    if (mapped > 255) mapped = 255;
                    return static_cast<uint8_t>(mapped);
                };
                if (axis == SDL_CONTROLLER_AXIS_LEFTX) m_stickLX = toByte(val);
                else if (axis == SDL_CONTROLLER_AXIS_LEFTY) m_stickLY = toByte(val);
                else if (axis == SDL_CONTROLLER_AXIS_RIGHTX) m_stickRX = toByte(val);
                else if (axis == SDL_CONTROLLER_AXIS_RIGHTY) m_stickRY = toByte(val);

                if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_RIGHTX) {
                    if (val < -DEADZONE) {
                        m_axisLeft = true;
                        m_axisRight = false;
                    } else if (val > DEADZONE) {
                        m_axisRight = true;
                        m_axisLeft = false;
                    } else if (std::abs(val) < RELEASE_ZONE) {
                        m_axisLeft = false;
                        m_axisRight = false;
                    }
                } else if (axis == SDL_CONTROLLER_AXIS_LEFTY || axis == SDL_CONTROLLER_AXIS_RIGHTY) {
                    if (val < -DEADZONE) {
                        m_axisUp = true;
                        m_axisDown = false;
                    } else if (val > DEADZONE) {
                        m_axisDown = true;
                        m_axisUp = false;
                    } else if (std::abs(val) < RELEASE_ZONE) {
                        m_axisUp = false;
                        m_axisDown = false;
                    }
                } else if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                    m_axisL2 = (val > 16000);
                } else if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    m_axisR2 = (val > 16000);
                }
                break;
            }

            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP: {
                if (!m_controller) {
                    bool down = (event.type == SDL_JOYBUTTONDOWN);
                    auto setBtn = [&](Button b, bool d) {
                        int bi = static_cast<int>(b);
                        if (d && !m_currentStates[bi]) m_justPressed[bi] = true;
                        m_currentStates[bi] = d;
                    };
                    switch (event.jbutton.button) {
                        case 0: setBtn(Button::B, down); break; // Bottom B
                        case 1: setBtn(Button::A, down); break; // Right A
                        case 2: setBtn(Button::Y, down); break; // Left Y
                        case 3: setBtn(Button::X, down); break; // Top X
                        case 4: setBtn(Button::L1, down); break;
                        case 5: setBtn(Button::R1, down); break;
                        // Brick fallback (order L to R: MENU - SELECT - START):
                        // nút trái = 8 (MENU), nút giữa = 6 (SELECT), nút phải = 7 (START)
                        case 6: setBtn(Button::SELECT, down); break;
                        case 7: setBtn(Button::START, down); break;
                        case 8: setBtn(Button::MENU, down); break;
                        default: break;
                    }
                }
                break;
            }

            case SDL_JOYHATMOTION: {
                m_hatUp = (event.jhat.value & SDL_HAT_UP);
                m_hatDown = (event.jhat.value & SDL_HAT_DOWN);
                m_hatLeft = (event.jhat.value & SDL_HAT_LEFT);
                m_hatRight = (event.jhat.value & SDL_HAT_RIGHT);
                break;
            }

            case SDL_JOYAXISMOTION: {
                int axis = event.jaxis.axis;
                int16_t val = event.jaxis.value;
                if (!m_controller) {
                    auto toByte = [](int16_t v) -> uint8_t {
                        int mapped = ((int)v + 32768) / 256;
                        if (mapped < 0) mapped = 0;
                        if (mapped > 255) mapped = 255;
                        return static_cast<uint8_t>(mapped);
                    };
                    if (axis == 0) m_stickLX = toByte(val);
                    else if (axis == 1) m_stickLY = toByte(val);

                    const int DEADZONE = 12000;
                    const int RELEASE_ZONE = 9000;

                    if (axis == 0) {
                        if (val < -DEADZONE) {
                            m_joyAxisLeft = true;
                            m_joyAxisRight = false;
                        } else if (val > DEADZONE) {
                            m_joyAxisRight = true;
                            m_joyAxisLeft = false;
                        } else if (std::abs(val) < RELEASE_ZONE) {
                            m_joyAxisLeft = false;
                            m_joyAxisRight = false;
                        }
                    } else if (axis == 1) {
                        if (val < -DEADZONE) {
                            m_joyAxisUp = true;
                            m_joyAxisDown = false;
                        } else if (val > DEADZONE) {
                            m_joyAxisDown = true;
                            m_joyAxisUp = false;
                        } else if (std::abs(val) < RELEASE_ZONE) {
                            m_joyAxisUp = false;
                            m_joyAxisDown = false;
                        }
                    }
                }
                break;
            }

            default: break;
        }
    }

    // Merge directional inputs from all sources (D-pad, Joystick axes, Hat, Keyboard)
    m_currentStates[static_cast<int>(Button::LEFT)] = m_dpadLeft || m_axisLeft || m_joyAxisLeft || m_hatLeft || m_keyLeft;
    m_currentStates[static_cast<int>(Button::RIGHT)] = m_dpadRight || m_axisRight || m_joyAxisRight || m_hatRight || m_keyRight;
    m_currentStates[static_cast<int>(Button::UP)] = m_dpadUp || m_axisUp || m_joyAxisUp || m_hatUp || m_keyUp;
    m_currentStates[static_cast<int>(Button::DOWN)] = m_dpadDown || m_axisDown || m_joyAxisDown || m_hatDown || m_keyDown;

    if (m_axisL2) m_currentStates[static_cast<int>(Button::L2)] = true;
    if (m_axisR2) m_currentStates[static_cast<int>(Button::R2)] = true;

    // Track button press start times for hold-to-repeat
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < static_cast<int>(Button::COUNT); ++i) {
        if (m_currentStates[i] && !m_previousStates[i]) {
            m_pressStartTime[i] = now;
            m_lastRepeatTime[i] = now;
            m_justPressed[i] = true; // latch lại: lần bấm nhanh không bị lọt giữa 2 vòng poll
        } else if (!m_currentStates[i]) {
            m_pressStartTime[i] = 0;
            m_lastRepeatTime[i] = 0;
        }
    }

    // Fast-path: Dispatch immediate HID report in BrickPad SESSION (0ms UI render lag)
    BrickPad::BrickPadManager::instance().onInputEvent();
}

bool InputManager::isButtonPressed(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;
    return m_currentStates[idx];
}

bool InputManager::isButtonJustPressed(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;

    // Edge latch: trả true 1 lần duy nhất rồi tự xóa (không lọt phím nhấn nhanh)
    if (m_justPressed[idx]) {
        const_cast<InputManager*>(this)->m_justPressed[idx] = false;
        return true;
    }

    // First transition from released to pressed
    if (m_currentStates[idx] && !m_previousStates[idx]) {
        return true;
    }

    // Auto-repeat for directional navigation when held
    if (btn == Button::UP || btn == Button::DOWN || btn == Button::LEFT || btn == Button::RIGHT) {
        if (m_currentStates[idx] && m_pressStartTime[idx] > 0) {
            uint32_t now = SDL_GetTicks();
            if (now - m_pressStartTime[idx] > 320 && now - m_lastRepeatTime[idx] > 110) {
                const_cast<InputManager*>(this)->m_lastRepeatTime[idx] = now;
                return true;
            }
        }
    }

    return false;
}

bool InputManager::isButtonRepeat(Button btn) const {
    int idx = static_cast<int>(btn);
    if (idx < 0 || idx >= static_cast<int>(Button::COUNT)) return false;
    // Edge first (also consumes the just-pressed latch).
    if (isButtonJustPressed(btn)) return true;
    // Then typematic while held.
    if (m_currentStates[idx] && m_pressStartTime[idx] > 0) {
        uint32_t now = SDL_GetTicks();
        if (now - m_pressStartTime[idx] > 400 && now - m_lastRepeatTime[idx] > 70) {
            const_cast<InputManager*>(this)->m_lastRepeatTime[idx] = now;
            return true;
        }
    }
    return false;
}

#if defined(__linux__)
static int s_evFds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static bool s_evInited = false;
static bool s_fnLeftLatched = false;
static bool s_fnRightLatched = false;

static void pollLinuxEvents() {
    if (!s_evInited) {
        s_evInited = true;
        for (int i = 0; i < 8; i++) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                s_evFds[i] = fd;
            }
        }
    }

    for (int i = 0; i < 8; i++) {
        if (s_evFds[i] < 0) continue;
        struct input_event evs[16];
        ssize_t bytes = read(s_evFds[i], evs, sizeof(evs));
        if (bytes > 0) {
            int count = (int)(bytes / sizeof(struct input_event));
            for (int k = 0; k < count; k++) {
                if (evs[k].type == 1) { // EV_KEY
                    if (evs[k].code == 59 && evs[k].value == 1) { // KEY_F1 (FN1)
                        s_fnLeftLatched = true;
                    } else if (evs[k].code == 60 && evs[k].value == 1) { // KEY_F2 (FN2)
                        s_fnRightLatched = true;
                    }
                }
            }
        }
    }
}
#endif

bool InputManager::isFnLeftTriggered() {
    if (isButtonPressed(Button::FN_LEFT) || isButtonJustPressed(Button::FN_LEFT)) return true;

#if defined(__linux__)
    pollLinuxEvents();
    if (s_fnLeftLatched) {
        s_fnLeftLatched = false;
        return true;
    }
#endif

    return false;
}

bool InputManager::isFnRightTriggered() {
    if (isButtonPressed(Button::FN_RIGHT) || isButtonJustPressed(Button::FN_RIGHT)) return true;

#if defined(__linux__)
    pollLinuxEvents();
    if (s_fnRightLatched) {
        s_fnRightLatched = false;
        return true;
    }
#endif

    return false;
}

} // namespace RomCloud

#include "switch_input_source.h"
#include "common/string_util.h"
#include "core/host.h"

#include "common/bitutils.h"

static const char* s_switch_button_names[] = {
  "A",     "B",     "X",        "Y",      "LStick",    "RStick",   "L",       "R",       "ZL",    "ZR",
  "Plus",  "Minus", "DPadLeft", "DPadUp", "DPadRight", "DPadDown", nullptr,   nullptr,   nullptr, nullptr,
  nullptr, nullptr, nullptr,    nullptr,  "LeftSL",    "LeftSR",   "RightSL", "RightSR",
};

static const char* s_switch_axis_names[] = {"LeftX", "LeftY", "RightX", "RightY"};
static const GenericInputBinding s_switch_generic_axis[][2] = {
  {GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},
  {GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},
  {GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight},
  {GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},
};

static u64 pseudo_buttons = HidNpadButton_StickLLeft | HidNpadButton_StickLUp | HidNpadButton_StickLRight |
                            HidNpadButton_StickLDown | HidNpadButton_StickRLeft | HidNpadButton_StickRUp |
                            HidNpadButton_StickRRight | HidNpadButton_StickRDown;

static const GenericInputBinding s_switch_generic_binding_button_mapping[] = {
  GenericInputBinding::Circle,   GenericInputBinding::Cross,   GenericInputBinding::Triangle,
  GenericInputBinding::Square,   GenericInputBinding::L3,      GenericInputBinding::R3,
  GenericInputBinding::L1,       GenericInputBinding::R1,      GenericInputBinding::L2,
  GenericInputBinding::R2,       GenericInputBinding::Start,   GenericInputBinding::Select,
  GenericInputBinding::DPadLeft, GenericInputBinding::DPadUp,  GenericInputBinding::DPadRight,
  GenericInputBinding::DPadDown, GenericInputBinding::Unknown, GenericInputBinding::Unknown,
  GenericInputBinding::Unknown,  GenericInputBinding::Unknown, GenericInputBinding::Unknown,
  GenericInputBinding::Unknown,  GenericInputBinding::Unknown, GenericInputBinding::Unknown,
  GenericInputBinding::Unknown,  GenericInputBinding::Unknown, GenericInputBinding::Unknown,
  GenericInputBinding::Unknown,
};

static_assert(std::size(s_switch_button_names) == SwitchInputSource::NUM_BUTTONS);
static_assert(std::size(s_switch_generic_binding_button_mapping) == SwitchInputSource::NUM_BUTTONS);

SwitchInputSource::SwitchInputSource() {}

SwitchInputSource::~SwitchInputSource() {}

bool SwitchInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  padConfigureInput(NUM_CONTROLLERS, HidNpadStyleSet_NpadStandard);
  padInitialize(&m_controllers[0].pad_state, HidNpadIdType_Handheld, HidNpadIdType_No1);
  padInitialize(&m_controllers[1].pad_state, HidNpadIdType_No2);
  padInitialize(&m_controllers[2].pad_state, HidNpadIdType_No3);
  padInitialize(&m_controllers[3].pad_state, HidNpadIdType_No4);

  for (u32 i = 0; i < 4; i++)
  {
    hidInitializeVibrationDevices(m_controllers[i].vibration_handles, 2, static_cast<HidNpadIdType>(i),
                                             HidNpadStyleSet_NpadStandard);
  }
  hidInitializeVibrationDevices(&m_controllers[0].vibration_handles[2], 2, HidNpadIdType_Handheld,
                                           HidNpadStyleTag_NpadHandheld);
  return true;
}
void SwitchInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) {}
void SwitchInputSource::Shutdown() {}

bool SwitchInputSource::ReloadDevices()
{
  PollEvents();
  return true;
}

void SwitchInputSource::PollEvents()
{
  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    padUpdate(&m_controllers[i].pad_state);

    bool is_connected = padIsConnected(&m_controllers[i].pad_state);
    if (is_connected)
    {
      std::string ident(StringUtil::StdStringFromFormat("P%u", i));
      if (!m_controllers[i].connected)
        Host::OnInputDeviceConnected(ident, ident);

      UpdateState(i);
    }
    else if (m_controllers[i].connected)
    {
      std::string ident(StringUtil::StdStringFromFormat("P%u", i));
      Host::OnInputDeviceDisconnected(ident);
    }

    m_controllers[i].connected = is_connected;
  }
}

void SwitchInputSource::UpdateState(u32 controller)
{
  for (u32 i = 0; i < 2; i++)
  {
    HidAnalogStickState state = padGetStickPos(&m_controllers[controller].pad_state, i);
    InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::Switch, controller, 2 * i + 0),
                               static_cast<float>(state.x) / JOYSTICK_MAX, GenericInputBinding::Unknown);
    InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::Switch, controller, 2 * i + 1),
                               static_cast<float>(state.y) / -JOYSTICK_MAX, GenericInputBinding::Unknown);
  }
  u64 buttons = padGetButtons(&m_controllers[controller].pad_state);
  buttons &= ~pseudo_buttons;
  buttons &= (1ull << NUM_BUTTONS) - 1;

  u64 buttons_diff = buttons ^ m_controllers[controller].buttons;
  m_controllers[controller].buttons = buttons;
  while (buttons_diff)
  {
    u32 button = CountTrailingZeros(buttons_diff);
    buttons_diff &= ~(1ull << button);
    float value = buttons & (1ull << button) ? 1.f : 0.f;
    InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::Switch, controller, button), value,
                               s_switch_generic_binding_button_mapping[button]);
  }
}

std::vector<std::pair<std::string, std::string>> SwitchInputSource::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> result;
  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    if (padIsConnected(&m_controllers[i].pad_state))
    {
      std::string ident(StringUtil::StdStringFromFormat("P%u", i));
      result.emplace_back(ident, ident);
    }
  }
  return result;
}

std::vector<InputBindingKey> SwitchInputSource::EnumerateMotors()
{
  std::vector<InputBindingKey> ret;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Switch;

  for (u32 i = 0; i < 4; i++)
  {
    key.source_index = i;
    key.source_subtype = InputSubclass::ControllerMotor;
    key.data = 0;
    ret.push_back(key);
    key.data = 1;
    ret.push_back(key);
  }

  return ret;
}

bool SwitchInputSource::GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
  for (u32 i = 0; i < NUM_AXIS; i++)
  {
    mapping->emplace_back(s_switch_generic_axis[i][0],
                          StringUtil::StdStringFromFormat("P%c/-%s", device[1], s_switch_axis_names[i]));
    mapping->emplace_back(s_switch_generic_axis[i][1],
                          StringUtil::StdStringFromFormat("P%c/+%s", device[1], s_switch_axis_names[i]));
  }
  for (u32 i = 0; i < NUM_BUTTONS; i++)
  {
    if (s_switch_generic_binding_button_mapping[i] != GenericInputBinding::Unknown)
      mapping->emplace_back(s_switch_generic_binding_button_mapping[i],
                            StringUtil::StdStringFromFormat("P%c/%s", device[1], s_switch_button_names[i]));
  }

  mapping->emplace_back(GenericInputBinding::SmallMotor, StringUtil::StdStringFromFormat("P%c/SmallMotor", device[1]));
  mapping->emplace_back(GenericInputBinding::LargeMotor, StringUtil::StdStringFromFormat("P%c/LargeMotor", device[1]));
  return true;
}

void SwitchInputSource::UpdateMotorState(InputBindingKey key, float intensity) {}
void SwitchInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                         float small_intensity)
{
  if (large_key.source_index != small_key.source_index)
    return;

  ControllerData& data = m_controllers[large_key.source_index];

  if (data.connected)
  {
    for (u32 i = 0; i < 2; i++)
    {
      HidVibrationValue value = {0};
      float intensity = i == 0 ? small_intensity : large_intensity;
      if (intensity != 0.f)
      {
        if (i == 0)
        {
          value.freq_low = 172.f;
          value.freq_high = 260.f;
          value.amp_low = value.amp_high = intensity * 0.9f;
        }
        else
        {
          value.freq_low = 195.f;
          value.freq_high = 195.f;
          value.amp_low = intensity * 0.8f;
          value.amp_high = intensity * 0.9f;
        }
      }
      else
      {
        value.freq_low = 160.0f;
        value.freq_high = 320.0f;
      }

      hidSendVibrationValue(data.vibration_handles[i], &value);
      if (large_key.source_index == 0)
        hidSendVibrationValue(data.vibration_handles[i + 2], &value);
    }
  }
}

std::optional<InputBindingKey> SwitchInputSource::ParseKeyString(const std::string_view& device,
                                                                 const std::string_view& binding)
{
  if (device.size() != 2 || device[0] != 'P' || !isdigit(device[1]))
    return std::nullopt;
  if (binding.empty())
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Switch;
  key.source_index = static_cast<u32>(device[1] - '0');

  if (binding[0] == '+' || binding[0] == '-')
  {
    for (u32 i = 0; i < NUM_AXIS; i++)
    {
      if (binding.substr(1) == s_switch_axis_names[i])
      {
        key.source_subtype = InputSubclass::ControllerAxis;
        key.modifier = binding[0] == '-' ? InputModifier::Negate : InputModifier::None;
        key.data = i;
        return key;
      }
    }
  }
  else if (binding.ends_with("Motor"))
  {
    key.source_subtype = InputSubclass::ControllerMotor;
    if (binding == "LargeMotor")
    {
      key.data = 0;
      return key;
    }
    else if (binding == "SmallMotor")
    {
      key.data = 1;
      return key;
    }
    return std::nullopt;
  }
  else
  {
    for (u32 i = 0; i < NUM_BUTTONS; i++)
    {
      if (s_switch_button_names[i] && binding == s_switch_button_names[i])
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = i;
        return key;
      }
    }
  }

  std::string device2(device);
  std::string binding2(binding);
  return std::nullopt;
}

TinyString SwitchInputSource::ConvertKeyToString(InputBindingKey key)
{
  TinyString ret;

  if (key.source_type == InputSourceType::Switch)
  {
    if (key.source_subtype == InputSubclass::ControllerAxis && key.data < NUM_AXIS)
    {
      ret.format("P{}/{}{}", static_cast<u32>(key.source_index), key.invert ? '-' : '+',
        s_switch_axis_names[key.data]);
    }
    else if (key.source_subtype == InputSubclass::ControllerButton && key.data < NUM_BUTTONS)
    {
      ret.format("P{}/{}", static_cast<u32>(key.source_index),
        s_switch_button_names[key.data]);
    }
    else if (key.source_subtype == InputSubclass::ControllerMotor)
    {
      ret.format("P{}/{}Motor", static_cast<u32>(key.source_index), key.data ? "Small" : "Large");
    }
  }

  return ret;
}

TinyString SwitchInputSource::ConvertKeyToIcon(InputBindingKey key)
{
  return {};
}

std::unique_ptr<InputSource> InputSource::CreateSwitchSource()
{
  return std::make_unique<SwitchInputSource>();
}

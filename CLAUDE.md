# stm32-games-console-elec2645

Group embedded project: games console on an STM32 Nucleo-L476RG (C, CMake, ST7789V2 LCD, joystick, buttons, buzzer, PWM). Three games in `game_1`, `game_2`, `game_3`; shared menu/input in `shared/`.

## Build
CMake presets in `CMakePresets.json` (arm-none-eabi toolchain). Build output goes to `build/` (gitignored). Hardware tests need the board: say so rather than claiming a hardware pass.

## Rules
- Embedded C: keep game loops non-blocking and check timer usage against `TIMER_USAGE_GUIDE.md` before touching PWM/TIM config.
- Do not edit generated CubeMX code outside `USER CODE` blocks, or `Drivers/`, or the `.ioc` file by hand.
- Group project: keep changes inside the game/module you were asked about.

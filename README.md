# ELEC2645 STM32 Games Console

A group embedded systems project developed for the ELEC2645 module using the **STM32 Nucleo-L476RG** microcontroller.

The project is a small games console featuring multiple games, joystick-based controls, button input, LCD display output, and modular game logic written in C.

## Project Overview

This repository contains three playable games developed for an STM32-based embedded games console. Each game is implemented in its own folder, with shared menu and input functionality used across the project.

## Games Included

| Game | Title | Status |
|---|---|---|
| Game 1 | Two-Player Chess | Complete |
| Game 2 | Rock-It Ralph | Complete |
| Game 3 | Tron Light Cycles | Complete |

## Repository Structure

```text
game_1/   Two-Player Chess
game_2/   Rock-It Ralph
game_3/   Tron Light Cycles
shared/   Shared menu system and input handling
Core/     STM32 HAL and generated project files
Drivers/  STM32 HAL drivers and CMSIS files

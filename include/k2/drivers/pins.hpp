// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// WITRN K2 board wiring: the one place that says which MCU pin does what.
// Pin numbers come from probing the board.
#pragma once
#include "k2/drivers/regs.hpp"

namespace k2::pins {

using reg::Pin;
using reg::Port;

// ---- ST7789V 240x240 LCD on SPI3 (8-bit simplex TX) ----
inline constexpr Pin lcd_cs    {Port::A, 15};  // chip select, active low
inline constexpr Pin lcd_sck   {Port::B, 3};   // SPI3 SCK (AF push-pull)
inline constexpr Pin lcd_dc    {Port::B, 4};   // data (1) / command (0)
inline constexpr Pin lcd_mosi  {Port::B, 5};   // SPI3 MOSI (AF push-pull)
inline constexpr Pin lcd_reset {Port::B, 6};   // panel reset, active low
inline constexpr Pin lcd_bl    {Port::B, 7};   // backlight, plain GPIO driven high (no PWM)

// ---- Buttons on the right edge, top to bottom: BACK, MINUS, OK, PLUS (active low) ----
inline constexpr Pin key_back  {Port::B, 14};  // back / menu / reset
inline constexpr Pin key_minus {Port::A, 14};  // previous / down (held at power-up: DFU)
inline constexpr Pin key_ok    {Port::A, 9};   // enter / confirm
inline constexpr Pin key_plus  {Port::A, 13};  // next / up

// ---- Type-C CC routing (GPIOC) ----
inline constexpr Pin cc_vconn_gnd {Port::C, 13};  // VCONN supply / ground control, low in normal use
inline constexpr Pin cc1_switch   {Port::C, 14};  // CC1 analog switch to the FUSB302, high = closed
inline constexpr Pin cc2_switch   {Port::C, 15};  // CC2 analog switch to the FUSB302, high = closed

// ---- Bit-banged I2C buses ----
inline constexpr Pin fusb_scl  {Port::B, 10};  // FUSB302, open drain, external pull-ups
inline constexpr Pin fusb_sda  {Port::B, 11};
inline constexpr Pin fusb_int  {Port::B, 12};  // FUSB302 INT_N, input with pull-up, EXTI12 falling edge (fusb302.cpp)
inline constexpr Pin ina_scl   {Port::B, 8};   // power monitor (INA2xx), push-pull
inline constexpr Pin ina_sda   {Port::B, 9};

// ---- Board switches, driven once at start-up; owned by k2::dpdm and k2::board ----
inline constexpr Pin sw_pa7    {Port::A, 7};   // D+/D- switch network (k2::dpdm), idle high
inline constexpr Pin sw_pb0    {Port::B, 0};   // k2::dpdm, idle low
inline constexpr Pin dp_dm_sw1 {Port::B, 1};   // k2::dpdm, idle high
inline constexpr Pin dp_dm_sw2 {Port::B, 2};   // k2::dpdm, idle low
inline constexpr Pin sw_pb13   {Port::B, 13};  // k2::board (function unknown), low
inline constexpr Pin sw_pb15   {Port::B, 15};  // k2::board (function unknown), low

// ---- Analog front end, ADC1 inputs; the scan order is in adc.cpp ----
inline constexpr Pin adc_aux    {Port::A, 0};  // ADC1_IN0, auxiliary rail behind an 11:1 divider
inline constexpr Pin adc_ripple {Port::A, 1};  // ADC1_IN1, AC-coupled VBUS ripple (high-speed capture)
inline constexpr Pin adc_dm     {Port::A, 2};  // ADC1_IN2, USB D- (also USART2 TX, k2::uart2)
inline constexpr Pin adc_dp     {Port::A, 3};  // ADC1_IN3, USB D+ (also USART2 RX)
inline constexpr Pin adc_cc1    {Port::A, 4};  // ADC1_IN4, CC1
inline constexpr Pin adc_cc2    {Port::A, 5};  // ADC1_IN5, CC2
inline constexpr Pin adc_ntc    {Port::A, 6};  // ADC1_IN6, board NTC

// ---- Test load ----
inline constexpr Pin load_pwm  {Port::A, 8};   // TIM1_CH1 (k2::load), VOOC wake load; duty 0 by default

// ---- USB full-speed lines ----
inline constexpr Pin usb_dm    {Port::A, 11};
inline constexpr Pin usb_dp    {Port::A, 12};

}  // namespace k2::pins

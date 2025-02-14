/*
 * This file is part of Cleanflight and Betaflight and EmuFlight.
 *
 * Cleanflight and Betaflight and EmuFlight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight and EmuFlight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#define TARGET_BOARD_IDENTIFIER "AIR3"

#define LED0_PIN                PB3
#define LED1_PIN                PB4

#define USE_BEEPER
#define BEEPER_PIN              PA12
#define BEEPER_INVERTED

#define USE_EXTI
#define USE_GYRO_EXTI
#define GYRO_1_EXTI_PIN         PC13
#define USE_MPU_DATA_READY_SIGNAL
#define ENSURE_MPU_DATA_READY_IS_LOW

#define USE_SPI
#define USE_SPI_DEVICE_2

#define GYRO_1_CS_PIN           PB12
#define GYRO_1_SPI_INSTANCE     SPI2

#define USE_GYRO
#define USE_GYRO_SPI_MPU6500
#define GYRO_1_ALIGN            CW0_DEG

#define USE_ACC
#define USE_ACC_SPI_MPU6500

#define USE_BARO
#define USE_BARO_SPI_BMP280

#define BARO_SPI_INSTANCE       SPI2
#define BARO_CS_PIN             PB5

#define USE_UART1
#define USE_UART2
#define USE_UART3
#define USE_SOFTSERIAL1
#define USE_SOFTSERIAL2
#define SERIAL_PORT_COUNT       5

#define USE_ESCSERIAL
#define ESCSERIAL_TIMER_TX_PIN  PA0  // (HARDARE=0,PPM)

#define SOFTSERIAL1_RX_PIN      PA6  // PWM 5
#define SOFTSERIAL1_TX_PIN      PA7  // PWM 6

#define SOFTSERIAL2_RX_PIN      PB6  // PWM 7
#define SOFTSERIAL2_TX_PIN      PB1  // PWM 8

#define UART1_TX_PIN            PA9
#define UART1_RX_PIN            PA10

#define UART2_TX_PIN            PA2
#define UART2_RX_PIN            PA3

#define UART3_TX_PIN            PB10 // PB10 (AF7)
#define UART3_RX_PIN            PB11 // PB11 (AF7)

#define DEFAULT_VOLTAGE_METER_SOURCE VOLTAGE_METER_ADC
#define USE_ADC
#define ADC_INSTANCE            ADC2
#define VBAT_ADC_PIN            PA4

#define DEFAULT_RX_FEATURE      FEATURE_RX_SERIAL
#define SERIALRX_PROVIDER       SERIALRX_SBUS
#define SERIALRX_UART           SERIAL_PORT_USART2
#define RX_CHANNELS_TAER

#define TARGET_IO_PORTA         0xffff
#define TARGET_IO_PORTB         0xffff
#define TARGET_IO_PORTC         0xffff
#define TARGET_IO_PORTD         0xffff
#define TARGET_IO_PORTF         (BIT(4))

#define USABLE_TIMER_CHANNEL_COUNT 14
#define USED_TIMERS             ( TIM_N(1) | TIM_N(2) | TIM_N(3) | TIM_N(4) | TIM_N(8) )

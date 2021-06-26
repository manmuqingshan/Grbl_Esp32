/*
    TrinamicUartDriverClass.cpp

    This is used for Trinamic UART controlled stepper motor drivers.

    Part of Grbl_ESP32
    2020 -	The Ant Team
    2020 -	Bart Dring

    TMC2209 Datasheet
    https://www.trinamic.com/fileadmin/assets/Products/ICs_Documents/TMC2209_Datasheet_V103.pdf

    Grbl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    Grbl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/
#include "TrinamicUartDriver.h"

#include "../Machine/MachineConfig.h"
#include "../Uart.h"
#include "../Report.h"  // info_serial
#include "../Config.h"  // TMC_UART is still a todo. For now I'll just include config.h for the defines

#include <TMCStepper.h>  // https://github.com/teemuatlut/TMCStepper
#include <atomic>

Uart tmc_serial(TMC_UART);

namespace Motors {

    bool TrinamicUartDriver::_uart_started = false;

    /* HW Serial Constructor. */
    TrinamicUartDriver::TrinamicUartDriver(uint16_t driver_part_number, uint8_t addr) : TrinamicBase(driver_part_number), _addr(addr) {}

    void TrinamicUartDriver::init() {
        if (!_uart_started) {
#ifdef LATER
            tmc_serial.setPins(TMC_UART_TX, TMC_UART_RX);
#endif
            tmc_serial.begin(115200, UartData::Bits8, UartStop::Bits1, UartParity::None);
            _uart_started = true;
        }
        _has_errors = hw_serial_init();

        link = List;
        List = this;

        if (_has_errors) {
            return;
        }

        // Display the stepper library version message once, before the first
        // TMC config message.  Link is NULL for the first TMC instance.
        if (!link) {
            info_serial("TMCStepper Library Ver. 0x%06x", TMCSTEPPER_VERSION);
        }

        config_message();

        tmcstepper->begin();

        _has_errors = !test();  // Try communicating with motor. Prints an error if there is a problem.

        read_settings();  // pull info from settings
        set_mode(false);

        // After initializing all of the TMC drivers, create a task to
        // display StallGuard data.  List == this for the final instance.
        if (List == this) {
            xTaskCreatePinnedToCore(readSgTask,    // task
                                    "readSgTask",  // name for task
                                    4096,          // size of task stack
                                    NULL,          // parameters
                                    1,             // priority
                                    NULL,
                                    SUPPORT_TASK_CORE  // must run the task on same core
            );
        }
    }

    bool TrinamicUartDriver::hw_serial_init() {
        if (_driver_part_number == 2209) {
            tmcstepper = new TMC2209Stepper(&tmc_serial, _r_sense, _addr);
            return false;
        }
        info_serial("Unsupported Trinamic motor p/n:%d", _driver_part_number);
        return true;
    }

    /*
        This is the startup message showing the basic definition. 
    */
    void TrinamicUartDriver::config_message() {  //TODO: The RX/TX pin could be added to the msg.
        info_serial("%s motor Trinamic TMC%d Step:%s Dir:%s Disable:%s UART%d Rx:%s Tx:%s Addr:%d R:%0.3f %s",
                    reportAxisNameMsg(axis_index(), dual_axis_index()),
                    _driver_part_number,
                    _step_pin.name().c_str(),
                    _dir_pin.name().c_str(),
                    _disable_pin.name().c_str(),
                    TMC_UART,
#ifdef LATER
                    pinName(TMC_UART_RX),
                    pinName(TMC_UART_TX),
#else
                    0,
                    0,
#endif
                    _addr,
                    _r_sense,
                    reportAxisLimitsMsg(axis_index()));
    }

    bool TrinamicUartDriver::test() {
        if (_has_errors) {
            return false;
        }

        switch (tmcstepper->test_connection()) {
            case 1:
                info_serial("%s Trinamic driver test failed. Check connection", reportAxisNameMsg(axis_index(), dual_axis_index()));
                return false;
            case 2:
                info_serial("%s Trinamic driver test failed. Check motor power", reportAxisNameMsg(axis_index(), dual_axis_index()));
                return false;
            default:
                // driver responded, so check for other errors from the DRV_STATUS register

                TMC2208_n ::DRV_STATUS_t status { 0 };  // a useful struct to access the bits.
                status.sr = tmcstepper->DRV_STATUS();

                bool err = false;

                // look for errors
                if (report_short_to_ground(status.s2ga, status.s2gb)) {
                    err = true;
                }

                if (report_over_temp(status.ot, status.otpw)) {
                    err = true;
                }

                if (report_short_to_ps(bit_istrue(status.sr, 12), bit_istrue(status.sr, 13))) {
                    err = true;
                }

                // XXX why not report_open_load(status.ola, status.olb) ?

                if (err) {
                    return false;
                }

                info_serial("%s Trinamic driver test passed", reportAxisNameMsg(axis_index(), dual_axis_index()));
                return true;
        }
    }

    /*
    Read setting and send them to the driver. Called at init() and whenever related settings change
    both are stored as float Amps, but TMCStepper library expects...
    uint16_t run (mA)
    float hold (as a percentage of run)
    */
    // XXX Identical to TrinamicDriver::read_settings()
    void TrinamicUartDriver::read_settings() {
        if (_has_errors) {
            return;
        }

        uint16_t run_i_ma = (uint16_t)(_run_current * 1000.0);
        float    hold_i_percent;

        if (_run_current == 0) {
            hold_i_percent = 0;
        } else {
            hold_i_percent = _hold_current / _run_current;
            if (hold_i_percent > 1.0) {
                hold_i_percent = 1.0;
            }
        }

        tmcstepper->microsteps(_microsteps);
        tmcstepper->rms_current(run_i_ma, hold_i_percent);

        init_step_dir_pins();
    }

    // XXX Identical to TrinamicDriver::set_homing_mode()
    bool TrinamicUartDriver::set_homing_mode(bool isHoming) {
        set_mode(isHoming);
        return true;
    }

    /*
    There are ton of settings. I'll start by grouping then into modes for now.
    Many people will want quiet and stallguard homing. Stallguard only run in
    Coolstep mode, so it will need to switch to Coolstep when homing
    */
    void TrinamicUartDriver::set_mode(bool isHoming) {
        if (_has_errors) {
            return;
        }

        TrinamicMode newMode = static_cast<TrinamicMode>(trinamicModes[isHoming ? _homing_mode : _run_mode].value);

        if (newMode == _mode) {
            return;
        }
        _mode = newMode;

        switch (_mode) {
            case TrinamicMode ::StealthChop:
                //info_serial("StealthChop");
                tmcstepper->en_spreadCycle(false);
                tmcstepper->pwm_autoscale(true);
                break;
            case TrinamicMode ::CoolStep:
                //info_serial("Coolstep");
                // tmcstepper->en_pwm_mode(false); //TODO: check if this is present in TMC2208/09
                tmcstepper->en_spreadCycle(true);
                tmcstepper->pwm_autoscale(false);
                break;
            case TrinamicMode ::StallGuard:  //TODO: check all configurations for stallguard
            {
                auto axisConfig     = config->_axes->_axis[this->axis_index()];
                auto homingFeedRate = (axisConfig->_homing != nullptr) ? axisConfig->_homing->_feedRate : 200;
                //info_serial("Stallguard");
                tmcstepper->en_spreadCycle(false);
                tmcstepper->pwm_autoscale(false);
                tmcstepper->TCOOLTHRS(calc_tstep(homingFeedRate, 150.0));
                tmcstepper->SGTHRS(constrain(_stallguard, 0, 255));
                break;
            }
            default:
                info_serial("Unknown Trinamic mode:d", _mode);
        }
    }

    /*
    This is the stallguard tuning info. It is call debug, so it could be generic across all classes.
    */
    void TrinamicUartDriver::debug_message() {
        if (_has_errors) {
            return;
        }

        uint32_t tstep = tmcstepper->TSTEP();

        if (tstep == 0xFFFFF || tstep < 1) {  // if axis is not moving return
            return;
        }
        float feedrate = st_get_realtime_rate();  //* settings.microsteps[axis_index] / 60.0 ; // convert mm/min to Hz

        info_serial("%s SG_Val: %04d   Rate: %05.0f mm/min SG_Setting:%d",
                    reportAxisNameMsg(axis_index(), dual_axis_index()),
                    tmcstepper->SG_RESULT(),  //    tmcstepper->sg_result(),
                    feedrate,
                    constrain(_stallguard, -64, 63));

        TMC2208_n ::DRV_STATUS_t status { 0 };  // a useful struct to access the bits.
        status.sr = tmcstepper->DRV_STATUS();

        // these only report if there is a fault condition
        report_open_load(status.ola, status.olb);
        report_short_to_ground(status.s2ga, status.s2gb);
        report_over_temp(status.ot, status.otpw);
        report_short_to_ps(bit_istrue(status.sr, 12), bit_istrue(status.sr, 13));

        // info_serial("%s Status Register %08x GSTAT %02x",
        //             reportAxisNameMsg(axis_index(), dual_axis_index()),
        //             status.sr,
        //             tmcstepper->GSTAT());
    }

    // XXX Identical to TrinamicDriver::set_disable()
    // this can use the enable feature over SPI. The dedicated pin must be in the enable mode,
    // but that can be hardwired that way.
    void TrinamicUartDriver::set_disable(bool disable) {
        if (_has_errors) {
            return;
        }

        if (_disabled == disable) {
            return;
        }

        _disabled = disable;

        _disable_pin.write(_disabled);

        if (_use_enable) {
            if (_disabled) {
                tmcstepper->toff(_toff_disable);
            } else {
                if (_mode == TrinamicMode::StealthChop) {
                    tmcstepper->toff(_toff_stealthchop);
                } else {
                    tmcstepper->toff(_toff_coolstep);
                }
            }
        }
        // the pin based enable could be added here.
        // This would be for individual motors, not the single pin for all motors.
    }

    // Configuration registration
    namespace {
        MotorFactory::InstanceBuilder<TMC2208> registration_2208("tmc_2208");
        MotorFactory::InstanceBuilder<TMC2209> registration_2209("tmc_2209");
    }
}

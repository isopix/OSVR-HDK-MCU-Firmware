/*
 * SerialStateMachine.c
 *
 * Created: 9/1/2013 5:07:35 PM
 *  Author: Sensics
 */

/*
             Main board software
*/

/*
  Copyright 2013  Sensics, Inc.

  This source code is copyrighted and can not be used without express
  written permission of Sensics, Inc.
*/


/** \file
 *
 *  Serial state machine file, The board has a control interface via a serial port (actually, virtual
 	serial port implemented over USB). This file implements a state machine for receiving the commands
	from this port and analyzing them
 */

// global variable definition

/* implements state machine:
	0: USB not connected
	1: Awaiting beginning of command ("#")
	2: Receiving command

	Once command has been received it is executed.
	If command exceeds certain length, machine reverts to awaiting beginning state and sends error message
*/

#include "config/GlobalOptions.h"
#include <asf.h>
#include <stdbool.h>
#include "string.h"
#include "conf_board.h"
#include "conf_timeout.h"
#include "uart.h"
#include "SerialStateMachine.h"
#include "config/my_hardware.h"

#ifndef DISABLE_NXP
#include "nxp/AVRHDMI.h"
#include "nxp/tmbslHdmiRx_types.h"
#include "nxp/tmdlHdmiRx.h"
#include "nxp/tmdlHdmiRx_cfg.h"
#include "nxp/tmbslTDA1997X_functions.h"
#include "nxp/tmbslTDA1997X_local.h"
#include "nxp/i2c.h"
#include "nxp/my_bit.h"
#endif

#include "stdio.h"
#include "DeviceDrivers/Solomon.h"
#include "boot.h"
#include "FPGA.h"
#include "DeviceDrivers/TI-TMDS442.h"
#include "DeviceDrivers/BNO070.h"
#include "Console.h"
#include "main.h"
#include <util/delay.h>

#define USBNotConnected		0
#define AwaitingCommand		1
#define ReceivingCommand	2
#define AwaitingCommandCompletion	3	// for commands that take a long time to complete such as contrast

uint8_t SerialState = USBNotConnected;
char CommandBuffer[MaxCommandLength+1];

bool CommandReady=false; // true if a command is ready to be executed
char CommandToExecute[MaxCommandLength+1];

char Msg[20]; // todo: remove after debug


typedef struct
{
    union {
        struct {
            uint8_t Signature[8];
            uint8_t Pad[EEPROM_PAGE_SIZE-8];
        };
        uint8_t Buffer[EEPROM_PAGE_SIZE];
    };
} EEPROM_type;

EEPROM_type EEPROM;

// todo: is this required?
uint8_t I2CAddress = 0; // selected I2C address
bool NXPLeftSide = true; // selected eye (left or right)


// todo: move USBActive as well as TRUE and FALSE
bool USBActive=false; // trueif USB is connected

uint8_t BufferPos=0; /* position of character to be received in new buffer. When command is completed,
					  this also shows the length of the command */
uint8_t ReadyBufferPos=0; // copy of BufferPos for command being executed

uint8_t HexDigitToDecimal(uint8_t CommandBufferIndex);
uint8_t HexPairToDecimal(uint8_t startIndex);
void Display_software_version(void);


// called once to reset state machine
void InitSerialState(void)
{
    SerialState = AwaitingCommand; // Ready to receive chars right now;
    CommandReady=false;
};


void ProcessIncomingChar(char CharReceived)

{
    switch (SerialState)
    {
    case USBNotConnected: // USB not ready, so don't process char
        break;
    case AwaitingCommand: // waiting for #
    {
        if (CharReceived == '#')
        {
            SerialState = ReceivingCommand;
            BufferPos = 0;
        }
        else if ((CharReceived == '\n') || (CharReceived == '\r')) // show user that the program is ready
        {
            WriteLn("");
            Write(">");
        };
        break;
    }
    case ReceivingCommand: // process normal character
    {
        if ((CharReceived == '\n') || (CharReceived == '\r'))
        {
            CommandBuffer[BufferPos] = '\0'; // terminate string
            // todo: add check whether CommandReady is already true and then decide what to do
            // move temp command to new buffer so that USB can start receiving next command
            memcpy(CommandToExecute,CommandBuffer,BufferPos+1);
            CommandReady=true;
            ReadyBufferPos=BufferPos;
            SerialState= AwaitingCommand; // wait for command to finish
            // todo: do we need PollStateMachine?
            //PollStateMachine=True;
        }
        else // any other character
        {
            if (BufferPos >= MaxCommandLength)
            {
                WriteLn("");
                WriteLn(";Command too long");
                Write(">");
                SerialState = AwaitingCommand;
                BufferPos=0;
            }
            else
            {
                CommandBuffer[BufferPos] = CharReceived;
                BufferPos++;
            }
        }
        break;
    };
    // todo: is this state really required?
    case AwaitingCommandCompletion:
    {
        Write(">");
        SerialState = AwaitingCommand; // ready for new command
        // todo: do we need PollStateMachine?
        //PollStateMachine=False;
    };
    };
};


void ProcessInfoCommands(void);
void ProcessBNO070Commands(void);
void ProcessSPICommand(void);
void ProcessI2CCommand(void);
void ProcessFPGACommand(void);

#ifndef DISABLE_NXP
void ProcessHDMICommand(void);
#endif


#ifdef TMDS422
    void ProcessTMDSCommand(void);
#endif

#define SIGNATURE_PAGE 0 // EEPROM page where Sensics signature is stored
/**
 * Set all values of a memory buffer to a given value
 */
static void set_buffer(uint8_t *buffer, uint8_t value)
{
    uint8_t i;

    for (i = 0; i < EEPROM_PAGE_SIZE; i++) {
        buffer[i] = value;
    }
}


/**
 * Check if an EEPROM page is equal to a memory buffer
 */
static bool is_eeprom_page_equal_to_buffer(uint8_t page_addr, uint8_t *buffer)
{
    uint8_t i;
    char Msg[10];

    for (i = 0; i < EEPROM_PAGE_SIZE; i++) {
        //WriteLn("+");
        if (nvm_eeprom_read_byte(page_addr * EEPROM_PAGE_SIZE + i) != buffer[i]) {
            WriteLn("---");
            sprintf(Msg,"%d %d %d",nvm_eeprom_read_byte(page_addr * EEPROM_PAGE_SIZE + i) ,buffer[i],i);
            WriteLn(Msg);
            return false;
        }
    }

    return true;
}



/*void eeprom_write_block (void *__src, uint32_t __dst, size_t __n)

{
	//nvm_write(INT_EEPROM,__dst,__src,__n);
};

void eeprom_read_block (void *__dst, const uint32_t __src, size_t __n)

{
	//nvm_read(INT_EEPROM, __src, __dst,__n);
};*/


// To do: move this to a util module

// converts hex digit to decimal equivalent. Works for upper and lower case. If not found, returns 0.
// accepts index of digit in command buffer as parameter


uint8_t HexDigitToDecimal(uint8_t CommandBufferIndex)

{
    char Digits[]="0123456789ABCDEF0000abcdef";
    uint8_t i;
    char CharToConvert=CommandToExecute[CommandBufferIndex];

    for (i = 0; i<26; i++)
        if (Digits[i] == CharToConvert)
        {
            if (i<16)
                return i;
            else
                return i-10;
        }
    return 0;
};


uint8_t HexPairToDecimal(uint8_t startIndex)

{
    return HexDigitToDecimal(startIndex) * 16 + HexDigitToDecimal(startIndex+1);
}


void Display_software_version(void)

{
    char OutString[12];

    Write("Version ");
    sprintf(OutString,"%d.%2.2d",MajorVersion,MinorVersion);
    Write(OutString);
    Write("  ");
    WriteLn(__DATE__);
#ifdef WITH_TRACKER
    Display_tracker_version();
#endif
#ifdef BNO070
	Write("Tracker:");
    sprintf(OutString,"%u.%u.%u.%lu",BNO070id.swVersionMajor,BNO070id.swVersionMinor,BNO070id.swVersionPatch,BNO070id.swBuildNumber);
    WriteLn(OutString);
#endif
}

void ProcessCommand(void)

{
    const char SENSICS[]="SENSICS\0";
    char OutString[12];
    eeprom_addr_t EEPROM_addr;

    if (ReadyBufferPos>0) // no need to process empty commands
    {
        switch (CommandToExecute[0])
        {
        case '?':
        {
            ProcessInfoCommands();
            break;
        };
#ifdef BNO070
		case 'B':
		case 'b':
		{
			ProcessBNO070Commands();
			break;
		};
#endif
        case 'S':
        case 's':
        {
            ProcessSPICommand();
            break;
        };
        case 'I':
        case 'i':
        {
            ProcessI2CCommand();
            break;
        };
        case 'F':
        case 'f':
        {
            ProcessFPGACommand();
            break;
        };
        case 't': // test commands for TMDS 422 switch
        case 'T':
        {
#ifdef TMDS422
            ProcessTMDSCommand();
#endif
            break;
        }
		#ifndef DISABLE_NXP

        case 'H': // HDMI commands
        case 'h':
        {
            ProcessHDMICommand();
            break;
        }
		#endif

        case 'P': // PWM settings
        case 'p':
        {
            set_pwm_values(HexPairToDecimal(2),HexPairToDecimal(2));
            break;
        }
        case 'E':  // Turn echo on/off. Also EEPROM functions
        case 'e':
        {
            switch (CommandToExecute[1])
            {
            case 'I': // write Sensics ID
            case 'i':
            {
                set_buffer(EEPROM.Buffer,0);
                memcpy(EEPROM.Signature,SENSICS,sizeof(SENSICS));
                nvm_eeprom_erase_and_write_buffer(SIGNATURE_PAGE*EEPROM_PAGE_SIZE, &EEPROM.Buffer[0], sizeof(EEPROM.Buffer));
                break;
            }
            case 'W': // write four bytes of data
            case 'w':
            {
                EEPROM_addr=HexPairToDecimal(2);
                nvm_eeprom_write_byte(EEPROM_addr,HexPairToDecimal(4));
                nvm_eeprom_write_byte(EEPROM_addr+1,HexPairToDecimal(6));
                nvm_eeprom_write_byte(EEPROM_addr+2,HexPairToDecimal(8));
                nvm_eeprom_write_byte(EEPROM_addr+3,HexPairToDecimal(10));
                break;
            }
            case 'V': // verify Sensics ID
            case 'v':
            {
                set_buffer(EEPROM.Buffer,0);
                memcpy(EEPROM.Buffer,SENSICS,sizeof(SENSICS));
                if (is_eeprom_page_equal_to_buffer(SIGNATURE_PAGE,EEPROM.Buffer))
                    WriteLn("Verified");
                else
                    WriteLn("Not verified");
                break;
            }
            case 'R': // read four bytes of data
            case 'r':
            {
                EEPROM_addr=HexPairToDecimal(2);
                for (int i=0; i<4; i++)
                {
                    sprintf(OutString,"%d ",nvm_eeprom_read_byte(EEPROM_addr + i));
                    Write(OutString);
                }
                WriteLn("");
                break;
            };
            };
            break;

        }
        case 'd':
        case 'D':
        {
            // debug commands
            WriteLn("Set debug level");
            SetDebugLevel(HexPairToDecimal(1));
            break;
        }
        default:
            WriteLn(";Unrecognized command");
        }
    }
    SerialState=AwaitingCommand; // todo: should this be here?
};

void ProcessInfoCommands(void)

// process commands that start with '?'

{
    char OutString[12];

    switch (CommandToExecute[1])
    {
    case 'V': // version
    case 'v':
    {
        Display_software_version();
        break;
    }
    case 'C': // clock
    case 'c':
    {
        // todo: add back clock
        // sprintf(OutString, "%2.2d:%2.2d:%2.2d:%2.2d", day,hour,minute,second);
        WriteLn(OutString);
        break;
    };
    case 'b': // bootloader
    case 'B':
    {
        if (memcmp(CommandToExecute+2,"1948",4)==0)
        {
            PrepareForSoftwareUpgrade();
        }
        break;
    }
    }
}

#ifdef BNO070
void ProcessBNO070Commands(void)
{
	char OutString[40];

	switch (CommandToExecute[1])
	{
		case 'D':
		case 'd':
		{
			switch (CommandToExecute[2])
			{
				case 'E':
				case 'e':
				{
					// #BDExx - BNO DCD Enable, set DCD enable flags
                    int flags = HexPairToDecimal(3);
					if (SetDcdEn_BNO070(flags)) {
                        sprintf(OutString, "Calibration flags set (%02x)", flags);
						WriteLn(OutString);
					}
					else {
						WriteLn("Failed.");
					}
					break;
				}
				case 'S':
				case 's':
				{
					// #BDS - BNO DCD Save
					if (SaveDcd_BNO070()) {
						WriteLn("DCD Saved.");
					}
					else {
						WriteLn("Failed.");
					}
					break;
				}
				case 'C':
				case 'c':
				{
					// #BDC = BNO DCD Clear
					if (ClearDcd_BNO070()) {
						WriteLn("DCD Cleared.");
					}
					else {
						WriteLn("Failed.");
					}
					break;
				}
			}
			break;
		}
		case 'M':
		case 'm':
		{
			switch (CommandToExecute[2])
			{
				case 'E':
				case 'e':
				{
					// #BMExx - BNO Mag Enable
					bool enabled = HexPairToDecimal(3) > 0;
                    if (MagSetEnable_BNO070(enabled)) {
						WriteLn(enabled ? "Mag Enabled." : "Mag Disabled.");
					}
					else {
						WriteLn("Failed.");
					}
					break;
				}
				case 'Q':
				case 'q':
				{
					// #BMS - BNO Mag Status
					uint8_t status = MagStatus_BNO070();  // 0 - Unreliable, 1 - Low, 2 - Medium, 3 - High Accuracy.
                    sprintf(OutString,"Mag Accuracy: %d", status);
                    WriteLn(OutString);
					break;
				}
			}
			break;
		}
		case 'S':
		case 's':
		{
			switch (CommandToExecute[2])
			{
				case 'Q':
				case 'q':
				{
					// #BSQ - BNO Stats Query
					BNO070_Stats_t stats;
					GetStats_BNO070(&stats);  // 0 - Unreliable, 1 - Low, 2 - Medium, 3 - High Accuracy.
					sprintf(OutString,"Resets: %lu", stats.resets);
					WriteLn(OutString);
					sprintf(OutString,"I2C Events: %lu", stats.events);
					WriteLn(OutString);
					sprintf(OutString,"Empty events:%lu", stats.empty_events);
					WriteLn(OutString);
					break;
				}
			}
			break;
		}
        case 'V':
        case 'v':
        {
            switch (CommandToExecute[2])
            {
                case 'V':
                case 'v':
                {
                    // #BVVxx log events to serial xx=0 turn off, all else = on
                    bool enabled = HexPairToDecimal(3) > 0;
                    SetDebugPrintEvents_BNO070(enabled);
                    WriteLn(enabled ? "enabled\n" : "disabled\n");
                    break;
                }
            }
        }
        case 'R':
        case 'r':
        {
            switch (CommandToExecute[2])
            {
                case 'I':
                case 'i':
                {
                    // #BRI
                    if (ReInit_BNO070()) {
                        WriteLn("Reinitialized");
                    } else {
                        WriteLn("Failed");
                    }
                    break;
                }
                case 'H':
                case 'h':
                {
                    // #BRH
                    if (Reset_BNO070())  {
                        WriteLn("Reset");
                    } else {
                        WriteLn("Failed");
                    }
                    break;
                }
            }
        }
	}
}
#endif


// send one or more bytes to the SPI interface and prints the received bytes

void ProcessSPICommand(void)

{
    uint16_t SolID,SolRegister;
    char OutString[12];



    switch (CommandToExecute[1])
    {

    case 'I': // init command
    case 'i':
    {
        if (CommandToExecute[2]=='1')
        {
            WriteLn("Init Sol1");
            init_solomon_device(Solomon1);
        }
#ifndef OSVRHDK
        else if (CommandToExecute[2]=='2')
        {
            WriteLn("Init Sol2");
            init_solomon_device(Solomon2);
        }
#endif
        else WriteLn("Bad sol ID");
        break;
    }
    case 'w': // Write Solomon register
    case 'W':
    {
        WriteLn("Write");
        uint16_t data;
        data=HexPairToDecimal(5);
        data=(data<<8) + HexPairToDecimal(7);
        if (CommandToExecute[2]=='1')
            write_solomon(Solomon1,HexPairToDecimal(3),data);
#ifndef OSVRHDK
        else if (CommandToExecute[2]=='2')
            write_solomon(Solomon2,HexPairToDecimal(3),data);
#endif
        else WriteLn("Wrong Solomon ID");
        break;
    }
    case 'r': // read Solomon ID or read register
    case 'R':
    {
        if (CommandToExecute[2]=='1')
        {
            if (ReadyBufferPos<4)  // no register number, just read ID
            {
                WriteLn("Read ID");
                SolID=read_Solomon_ID(Solomon1);
                sprintf(OutString,"Id %x",SolID);
            }
            else
            {
                write_solomon(Solomon1,0xd4,0xfa); // about to read register
                SolRegister=read_solomon(Solomon1, HexPairToDecimal(3));
                sprintf(OutString,"Read: %x",SolRegister);
            }
        }
#ifndef OSVRHDK
        else if (CommandToExecute[2]=='2')
        {
            if (ReadyBufferPos<4)  // no register number, just read ID
            {
                WriteLn("Read ID");
                SolID=read_Solomon_ID(Solomon2);
                sprintf(OutString,"Id %x",SolID);
            }
            else
            {
                write_solomon(Solomon2,0xd4,0xfa); // about to read register
                SolRegister=read_solomon(Solomon2, HexPairToDecimal(3));
                sprintf(OutString,"Read: %x",SolRegister);
            }
        }
#endif
        else
        {
            WriteLn("Wrong Solomon ID");
            return;
        }
        WriteLn(OutString);
        break;
    }
    case 'u':
    case 'U':
    {
        if (CommandToExecute[2]=='1')
            raise_sdc(Solomon1);
#ifndef OSVRHDK
        else if (CommandToExecute[2]=='2')
            raise_sdc(Solomon2);
#endif
        else
            WriteLn("Wrong Solomon ID");
        break;
    }
	case 'n':
	case 'N':
	{
		WriteLn("Display on");
		DisplayOn(Solomon1);
		break;
	}
	case 'f':
	case 'F':
	{
		WriteLn("Display off");
		DisplayOff(Solomon1);
		break;
	}
    case 'd':
    case 'D':
    {
        if (CommandToExecute[2]=='1')
            lower_sdc(Solomon1);
#ifndef OSVRHDK
        else if (CommandToExecute[2]=='2')
            lower_sdc(Solomon2);
        else if (CommandToExecute[2]=='3')
        {
            WriteLn("OE high");
            ioport_configure_pin(SPI_Mux_OE,IOPORT_DIR_OUTPUT | IOPORT_INIT_LOW);
            gpio_set_pin_high(SPI_Mux_OE);
        }
        else if (CommandToExecute[2]=='4')
            gpio_toggle_pin(SPI_Mux_OE);
#endif
        else
            WriteLn("Wrong Solomon ID");
        break;
    }
    case '0': // solomon reset
    {
        Solomon_Reset(HexDigitToDecimal(2));
        break;
    }
    case '1': // display reset
    {
        if (HexDigitToDecimal(2)==1)
        {
            powercycle_display(Solomon1);
            //init_solomon_device(Solomon1);
        }
#ifndef OSVRHDK
        else if (HexDigitToDecimal(2)==2)
        {
            powercycle_display(Solomon2);
            //init_solomon_device(Solomon2);
        }
#endif
        else
            WriteLn("Wrong ID");
        break;
    };

    }

    WriteLn("");
}


// send one or more bytes to the I2C interface and prints the received bytes

void ProcessI2CCommand(void)

{
    uint8_t TxByte, RxByte, Num, Page;
    bool Result = false;

    char OutString[14];

    switch (CommandToExecute[1])
    {
    case 'D': // Select I2C device
    case 'd':
    {
        I2CAddress = HexPairToDecimal(2); // since command type is in index 1, start at 2
        WriteLn(";Address received");
        break;
    }

    case 'R':  // Receive Data Command
    case 'r':
    {

        Num= HexPairToDecimal(2); // num of bytes to receive

        // todo: replace with real stuff
        RxByte=0;
        //RxByte=I2CReadRegister(I2CAddress,Num);
        sprintf(OutString, "R %x ", RxByte);
        Write(OutString);
        break;

    }

	#ifndef DISABLE_NXP

    case 'f':
    case 'F': // find available I2C addresses
    {
        for (int i=0x80; i<0xA0; i=i+2)
        {
            sprintf(Msg,"Trying %x",i);
            WriteLn(Msg);
            if (I2CReadRegister(i,0))
                WriteLn("Success");
            _delay_ms(100);
        }
        break;
    }

	#endif

#ifdef BNO070
	case 'B':
	case 'b': // BNO commands
	{
        switch (CommandToExecute[2]) // sub-commands for NXP
        {
			// to read a 1-byte Hex value after the subcommand use something like param=HexToDecimal(3)
			// to read a 2-byte Hex value after the subcommand use something like param=HexPairToDecimal(3)
			case 'T':  // tare
			case 't':
			{
				Tare_BNO070();
			}
			break;
		}
	}
	break;

#endif

#ifndef DISABLE_NXP

    case 'N': // NXP commands
    case 'n':
    {
        switch (CommandToExecute[2]) // sub-commands for NXP
        {
        case 'W': // write register
        case 'w':
        {
            Num = HexPairToDecimal(3); // Register number
            TxByte = HexPairToDecimal(5); // byte to send
            if (NXPLeftSide==false)
                Result=i2cWriteRegister(NXP_1_ADDR, Num,TxByte);
#ifndef OSVRHDK
            else
                Result=i2cWriteRegister(NXP_2_ADDR, Num,TxByte);
#endif
            if (!Result)
                Write(";Write failed");
            break;
        }

        case 'R': // read register
        case 'r':
        {
            Num = HexPairToDecimal(3); // Register number
            if (NXPLeftSide==false)
                RxByte=NXPReadRegister(NXP_1_ADDR, Num);
#ifndef OSVRHDK
            else
                RxByte=NXPReadRegister(NXP_2_ADDR, Num);
#endif
            sprintf(OutString, "%x ", RxByte);
            Write(OutString);
            break;
        };
        case 'X': // read register through NXP library
        case 'x':
        {
#define ACC_REG(a,p)            (UInt16)(((p)<<8)|(a))
            UInt32 errCode;

            Num = HexPairToDecimal(3); // Register number
            Page = HexPairToDecimal(5);
            if (NXPLeftSide==false)
                errCode = tmbslTDA1997XReadI2C(0, ACC_REG(Num,Page), 1, &RxByte);
            //RxByte=NXPReadRegister(NXP_1_ADDR, Num);
            else
                errCode = tmbslTDA1997XReadI2C(1, ACC_REG(Num,Page), 1, &RxByte); /// temp. Should be unit 1
            //RxByte=NXPReadRegister(NXP_2_ADDR, Num);
            if (errCode != TM_OK)
                sprintf(OutString, "Err %ld ", errCode);
            else
                sprintf(OutString, "%x ", RxByte);
            Write(OutString);
            delay_ms(1);
            if (NXPLeftSide==false)
                errCode = tmbslTDA1997XReadI2C(0, ACC_REG(Num,Page), 1, &RxByte);
            //RxByte=NXPReadRegister(NXP_1_ADDR, Num);
            else
                errCode = tmbslTDA1997XReadI2C(1, ACC_REG(Num,Page), 1, &RxByte); /// temp. Should be unit 1
            //RxByte=NXPReadRegister(NXP_2_ADDR, Num);
            if (errCode != TM_OK)
                sprintf(OutString, "Err %ld ", errCode);
            else
                sprintf(OutString, "%x ", RxByte);
            Write(OutString);
            delay_ms(1);
            if (NXPLeftSide==false)
                errCode = tmbslTDA1997XReadI2C(0, ACC_REG(Num,Page), 1, &RxByte);
            //RxByte=NXPReadRegister(NXP_1_ADDR, Num);
            else
                errCode = tmbslTDA1997XReadI2C(1, ACC_REG(Num,Page), 1, &RxByte); /// temp. Should be unit 1
            //RxByte=NXPReadRegister(NXP_2_ADDR, Num);
            if (errCode != TM_OK)
                sprintf(OutString, "Err %ld ", errCode);
            else
                sprintf(OutString, "%x ", RxByte);
            Write(OutString);
            break;
        };
        case 'S': // side
        case 's':
        {
            NXPLeftSide=(CommandBuffer[3]=='0');
            break;
        };
        case '0':
        {
            NXPSuspend();
            break;
        }
        case '1':
        {
            NXPResume();
            break;
        }
        };
        break;
    };

#endif
    }

    WriteLn("");
}


void ProcessFPGACommand(void)

// #FnRxx reads address XX from FPGA n
// #FnWxxyy writes value YY to address XX in FPGA n

{
    uint8_t FPGANum,Addr,Data;

    if (CommandToExecute[1]=='2')
        FPGANum=2;
    else if (CommandToExecute[1]=='1')
        FPGANum=1;
    else
    {
        WriteLn("Bad command");
        return;
    }
    switch (CommandToExecute[2])
    {
    case 'r':
    case 'R':
    {
        WriteLn("Read");
        Addr=HexPairToDecimal(3);
        sprintf(Msg,"%x = %x",Addr,FPGA_read(FPGANum,Addr));
        WriteLn(Msg);
        WriteLn("After");
        break;
    }
    case 'w':
    case 'W':
    {
        WriteLn("Write");
        Addr=HexPairToDecimal(3);
        Data=HexPairToDecimal(5);
        FPGA_write(FPGANum,Addr,Data);
        break;
    }
    case 's':
    case 'S': // toggle side by side
    {
        WriteLn("Toggle side by side");
#ifndef OSVRHDK
        ioport_toggle_pin_level(Side_by_side_A);
#endif
        ioport_toggle_pin_level(Side_by_side_B);
        break;
    }
    case '0':
    {
        WriteLn("FPGA Reset");
        FPGA_reset();
        break;
    }
	case 'l':
	case 'L':
	{
#ifdef OSVRHDK
		if (ioport_get_pin_level(FPGA_unlocked))
			WriteLn("FPGA unlocked");
		else
			WriteLn("FPGA locked");
#endif
		break;
	}

    }

}

#ifndef DISABLE_NXP

void ProcessHDMICommand(void)

{
    switch (CommandToExecute[1])
    {
    case 'I':
    case 'i':
    {
        WriteLn(";Init HDMI");
        Init_HDMI();
        WriteLn(";End init");
        break;
    }
    case 'E':
    case 'e':
    {
        WriteLn("Pulse HPD");
        tmdlHdmiRxManualHPD(0,BSLHDMIRX_HPD_PULSE);
        break;
    }
    case 'H':
    case 'h':
    {
        WriteLn("Pulse HPD 1sec");
        tmdlHdmiRxManualHPD(0,TMDL_HDMIRX_HPD_HIGH);
        _delay_ms(1000);
        tmdlHdmiRxManualHPD(0,TMDL_HDMIRX_HPD_LOW);
        break;
    }

    case 'R':
    case 'r':
    {
        Report_HDMI_status();
        break;
    }
    case 'T':
    case 't':
    {
        HDMI_task=true;
        break;
    }
    case 'S':
    case 's':
    {
        HDMIShadow=(CommandToExecute[2]=='1');
        if (HDMIShadow)
            WriteLn("Shadow on");
        break;
    }
    case 'V':
    case 'v':
    {
        uint8_t i;
        // move both NXP to page 0
        i2cWriteRegister(NXP_1_ADDR, 0xff, 0);
#ifndef OSVRHDK
        i2cWriteRegister(NXP_2_ADDR, 0xff, 0);
#endif
        for (i=0; i<10; i++)
        {
            NXPReadRegister(NXP_1_ADDR,0);
#ifndef OSVRHDK
            NXPReadRegister(NXP_2_ADDR,0);
#endif
        }
        break;
    }
    case '0':
    {
        HDMI_Reset(HexDigitToDecimal(2));
        break;
    }
    case 'M':
    case 'm':
    {
        if (CommandBuffer[2]=='0')
        {
            WriteLn(";Program MTP");
            _delay_ms(50);
            ProgramMTP0();
        }
		#ifndef OSVRHDK
        if (CommandBuffer[2]=='1')
        {
            WriteLn(";Program MTP1");
            _delay_ms(50);
            ProgramMTP1();
        }
		#endif
        break;
    }
    }
}

#endif

#ifdef TMDS422
void ProcessTMDSCommand(void)

{
    uint8_t Temp;

    switch (CommandToExecute[1])
    {
    case 'r':
    case 'R': // read status
    {
        if (ReadTMDS422Status(HexDigitToDecimal(2),&Temp))
        {
            sprintf(Msg,"Read:%x",Temp);
            WriteLn(Msg);
        }
        else
            WriteLn("Read failed");
        break;
    }
    case 'w':
    case 'W': // write
    {
        HDMI_config(HexPairToDecimal(2), HexPairToDecimal(4));
        break;
    }
    case 's': // set input status
    case 'S':
    {
        Temp=HexPairToDecimal(2);
        SetInputStatus(Temp);
        break;
    }
    case 't': // start task
    case 'T':
    {
        HDMISwitch_task=true;
#ifdef TMDS422
        //TMDS_422_Task();
        //timeout_start_periodic(	TMDS_422_Timeout, 1);
#endif
        break;
    }
	case 'p':
	case 'P':
	{
		WriteLn("Toggle backlight");
		ioport_toggle_pin_level(PWM_A);
		ioport_toggle_pin_level(PWM_B);
		break;
	}
    case 'i':
    case 'I':
    {
        WriteLn("Init switch");
        InitHDMISwitch();
        break;
    }
    default:
    {
        WriteLn(";Unknown command");
        break;
    }
    }
}
#endif
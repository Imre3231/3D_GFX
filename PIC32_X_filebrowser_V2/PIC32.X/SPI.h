 

void InitPorts ()
{

 /*************************************************************************
REMAP INPUTS:
[module name]Rbits.[module name] = 4-bit binary code for [pin name]
**************************************************************************/

SDI2Rbits.SDI2R = 0b0001; // (RPG7 = RG7 input) 

SDI1Rbits.SDI1R = 0b0000;   //SDI1
/*************************************************************************
REMAP OUTPUT OUTPUT PINS TO MODULE:
[pin name]Rbits.[pin name] = 4-bit binary code for [module name]
**************************************************************************/

 INT1R = 0b0000; //Pin F13 = INT1

RPD3Rbits.RPD3R = 0b0101;  //  //SDO1 

RPG8Rbits.RPG8R = 0b0110;  // (0110 = SDO2) to RG8   
 

 

     ANSELE = 0X0;
     ANSELA = 0X0000;                   // PORTA ANSELA Cleared = Digital
    ANSELB = 0b0;  // PORTB ANSELB Cleared = Digital (Set RB15/AN10/CSS10 "frequency-Input" as ANALOG input)
    ANSELD = 0X0000;
    ANSELG = 0;                   // PORTG ANSELG Cleared = Digital
    TRISA=0X00;
    TRISB=0;
    TRISC=0X0;
      TRISG=0x0;
       TRISE = 0X00; //0 as Output PIN
    TRISD=0;
  TRISF=0;
  
    TRISDbits.TRISD2= 1; // SDI6 

    TRISDbits.TRISD0= 1; // SDI6 
  TRISDbits.TRISD12  =0 ; //  XCS ------Chip_select cofigure pin as output
  TRISDbits.TRISD13  =0 ; //  XRESET----Hardware_Reset   MP3_RST pin, cofigure pin as output      
  TRISDbits.TRISD4  =1 ; //  DREQ------Data_Request     // Configure DREQ as input    
  TRISDbits.TRISD5  =0 ; //  BSYNC-----SDI data is synchronized with a rising edge of BSYNC, cofigure pin as output
TRISGbits.TRISG7= 1;  //SDI2
    


TRISAbits.TRISA2= 1;
TRISAbits.TRISA3= 1;
TRISAbits.TRISA4= 1;








}




void Init_Spi(void)


{
    
    
    
    
       SPI1CONbits.ON = 0; // Turn off SPI2 before configuring
    SPI1CONbits.MSTEN = 1; // Enable Master mode
    SPI1CONbits.CKP = 0; // Clock signal is active low, idle state is high
    SPI1CONbits.CKE = 1; // Data is shifted out/in on transition from idle (high) state to active (low) state
    SPI1CONbits.SMP = 0; // Input data is sampled at the end of the clock signal
    SPI1CONbits.MODE16 = 0; // Do not use 16-bit mode
    SPI1CONbits.MODE32 = 0; // Do not use 32-bit mode (combines with the above line to activate 8-bit mode)
    SPI1BRG = 0; // fastest SPI1 clock for FPGA AVI streaming
    SPI1CONbits.ENHBUF = 0; // DMA stream uses one byte per SPI1 TX-empty request
    SPI1CON2bits.IGNROV = 1; // keep TX running even when RX overflows during TX-only DMA
    SPI1CON2bits.IGNTUR = 1;
    SPI1CONbits.ON = 1; // Configuration is done, turn on SPI2 peripheral
 

    
        SPI2CONbits.ON = 0; // Turn off SPI2 before configuring
    SPI2CONbits.MSTEN = 1; // Enable Master mode
    SPI2CONbits.CKP = 0; // Clock signal is active low, idle state is high
    SPI2CONbits.CKE = 1; // Data is shifted out/in on transition from idle (high) state to active (low) state
    SPI2CONbits.SMP = 0; // Input data is sampled at the end of the clock signal
    SPI2CONbits.MODE16 = 0; // Do not use 16-bit mode
    SPI2CONbits.MODE32 = 0; // Do not use 32-bit mode (combines with the above line to activate 8-bit mode)
    SPI2BRG = 200; // Set Baud Rate Generator to 0 [discussed below]
    SPI2CONbits.ENHBUF = 0; // Disables Enhanced Buffer mode [also discussed below]
    SPI2CONbits.ON = 1; // Configuration is done, turn on SPI2 peripheral
    
    
    
    
    
    
    
    
SDI2Rbits.SDI2R = 0b0001; // (RPG7 = RG7 input) 

SDI1Rbits.SDI1R = 0b0000;   //SDI1
 

RPD3Rbits.RPD3R = 0b0101;  //  //SDO1 

RPG8Rbits.RPG8R = 0b0110;  // (0110 = SDO2) to RG8     
    
    
    
    
    
    
    
}






unsigned char WriteSPI( unsigned char data);
unsigned char WriteSPI6( unsigned char data);



unsigned char WriteSPI3( unsigned char data)
{
SPI1BUF=data; // write to buffer for TX
while( !SPI1STATbits.SPIRBF); // wait transfer complete
return SPI1BUF; // read the received value
}// writeSPI


unsigned char WriteSPI( unsigned char data)
{
SPI2BUF=data; // write to buffer for TX
while( !SPI2STATbits.SPIRBF); // wait transfer complete
return SPI2BUF; // read the received value
}// writeSPI



unsigned char ReadSPI(void)
{
    unsigned char data;
    data= WriteSPI(0xff);
    return data;
   
}



unsigned char WriteSPI2( unsigned char data)
{
SPI1BUF=data; // write to buffer for TX
while( !SPI1STATbits.SPIRBF); // wait transfer complete
return SPI1BUF; // read the received value
}// writeSPI




unsigned char ReadSPI2(void)
{
    unsigned char data;
    data= WriteSPI2(0xff);
    return data;
   
}
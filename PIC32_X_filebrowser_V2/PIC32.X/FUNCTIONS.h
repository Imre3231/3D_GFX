
 
  








void Memory_Wr(unsigned long Address, unsigned int DATA)
{
    unsigned char ii=0;  
    unsigned char i;
        WriteSPI3((Address>>16));
        WriteSPI3((Address>>8));
        WriteSPI3(Address);
      
        
        WriteSPI3((DATA>>8));
 
         WriteSPI3(DATA);
  
            MEMWR=1;
    
              
            MEMWR=0;
   
     }






 

 void Pixel(unsigned int x, unsigned int y, unsigned int data)
{
    unsigned long a;
    unsigned long b;
    unsigned long Address;
 
    a=x;
    b=y;
    Address=(a+(b*320));
    
        WriteSPI3((Address>>16));
        WriteSPI3((Address>>8));
        WriteSPI3(Address);
      
        
        WriteSPI3(( data>>8));
 
         WriteSPI3( data);
  
            MEMWR=1;
    
              
            MEMWR=0;
    
  
    
}

  
  
 
  

 void text3 (unsigned char *buffer,unsigned int x,unsigned int y,unsigned char spaceing,unsigned int color1,unsigned int color2)
{
    unsigned char WIDTH; 
    unsigned char count=0;
    CHARACTER_POSITON=0;
    
    
    
    while(*buffer)              // Write data to LCD up to null
    {
        
         
        GLCD_FONT2((*buffer-32),(((CHARACTER_POSITON))+x) ,y,color1,color2,1,Times_New_Roman16x16);      // Write character to LCD
        
         WIDTH = CHARACTER_WIDTH((*buffer-32),Times_New_Roman16x16);       
         CHARACTER_POSITON=CHARACTER_POSITON+WIDTH+spaceing;
        buffer++;  // Increment buffer 
        count++;
        
        
 
    }
 }
 
 

 void text4 (unsigned char *buffer,unsigned int x,unsigned int y,unsigned int spaceing ,unsigned int color1,unsigned int color2)
{
    unsigned char WIDTH; 
    unsigned char count=0;
    CHARACTER_POSITON=0;
    while(*buffer)              // Write data to LCD up to null
    {
        
         
        GLCD_FONT2((*buffer-32),(((CHARACTER_POSITON))+x) ,y,color1,color2,1,Tahoma44x50);      // Write character to LCD
        
         WIDTH = CHARACTER_WIDTH((*buffer-32),Tahoma44x50);       
         CHARACTER_POSITON=CHARACTER_POSITON+WIDTH+spaceing;
        buffer++;  // Increment buffer 
        count++;
    }
    
    
}

 
 
 
 
 








void tohex(unsigned char in,  char * out)
{
  
    const char * hex = "0123456789ABCDEF";
    char * pout = out;
    
    
        
        pout[0] = hex[(in>>4) & 0xF];
        pout[1] = hex[ in     & 0xF];
       
        pout[2] = 0;
   
}



void tostring(unsigned int Number ,  char * out)
{
    
   
    char * pout = out;
    
        pout[2] =((Number/1)%10)+48;
        pout[1] = ((Number/10)%10)+48;;
        pout[0] = ((Number/100)%10)+48;;
        pout[3] = 0;
   
}

void tostring2(unsigned int Number ,  char * out)
{
    
 
        char * pout = out;
        pout[4] = ((Number/1)%10)+48;
        pout[3] = ((Number/10)%10)+48;
        pout[2] = ((Number/100)%10)+48;
        pout[1] = ((Number/1000)%10)+48;;
        pout[0] = ((Number/10000)%10)+48;;
        pout[5] = 0;
   
}



void tostring3(unsigned long Number ,  char * out)
{
    
 
        char * pout = out;
        pout[9] = ((Number/1)%10)+48;
        pout[8] = ((Number/10)%10)+48;
        pout[7] = ((Number/100)%10)+48;
        pout[6] = ((Number/1000)%10)+48;
        pout[5] = ((Number/10000)%10)+48;
        pout[4] = ((Number/100000)%10)+48;
        pout[3] = ((Number/1000000)%10)+48;
        pout[2] = ((Number/10000000)%10)+48;
        pout[1] = ((Number/100000000)%10)+48;
        pout[0] = ((Number/1000000000)%10)+48;
        
        pout[10] = 0;
   
}




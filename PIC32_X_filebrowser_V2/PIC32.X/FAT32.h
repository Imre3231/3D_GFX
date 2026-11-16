/*
 * FAT32.h
 *
 *  Created on: 2020.04.02.
 *      Author: DR
 */







/*
00h	1 bytes	Ordinal field
01h	2 bytes	Unicode Character 1
03h	2 bytes	Unicode Character 2
05h	2 bytes	Unicode Character 3
07h	2 bytes	Unicode Character 4
09h	2 bytes	Unicode Character 5
0Bh	1 bytes	Flag byte
0Ch	1 bytes	Reserved - Always 00h
0Dh	1 bytes	Checksum
0Eh	2 bytes	Unicode Character 6
10h	2 bytes	Unicode Character 7
12h	2 bytes	Unicode Character 8
14h	2 bytes	Unicode Character 9
16h	2 bytes	Unicode Character 10
18h	2 bytes	Unicode Character 11
1Ah	2 bytes	Always 0000h
1Ch	2 bytes	Unicode Character 12
1Eh	2 bytes	Unicode Character 13
*/

 unsigned int Data1;
unsigned long Data2;
unsigned int Master_Boot_Record=8192;

unsigned char Filename_Table[50] ;


//unsigned char Longfilename_Character[14]={ 0x1E,0x1C,0x1A,0x18,0x16,0x14,0x12,0x10,0x0E,0x09,0x07,0x05,0x03,0x01 } ;
unsigned char Longfilename_Character[14]={ 0x01,0x03,0x05,0x07,0x09,0x0E,0x10,0x12,0x14,0x16,0x18,0x1C,0x1E } ;

unsigned char txt_FAT32[17]={0x20,0x4E,0x41,0x4D,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x33,0x32,0x20,0x20,0x20} ;



 unsigned int File_Offset[900];

unsigned int Lineoffset;

unsigned int Bytes_Per_Sector;

unsigned int Sectors_Per_Cluster;
unsigned int Number_of_FATs;

unsigned long Hidden_Sectors;
unsigned long Sectors_Per_FAT;
unsigned long Root_Cluster_Number;
unsigned int  Reserved_Sectors;

unsigned long Root_Directory;
unsigned long Data_Start_Sector;
unsigned int Number_Of_Files_Indirectory;

unsigned long Reed_File_Size(unsigned long Directory_Entry, unsigned int Filenumber );
unsigned char Reed_File_Type(unsigned long Directory_Entry, unsigned int Filenumber );



unsigned int Search_File_Indirectory(unsigned long Directory_Entry,unsigned char *buff,unsigned int *buff2);

unsigned char Reed_Long_Filename(unsigned long Directory_Entry, unsigned int Filenumber, char *txtbuff);

unsigned char Reed_Byte(unsigned char offset,unsigned char *buff);
unsigned int  Reed_Word(unsigned char offset,unsigned char *buff);
unsigned long Reed_Dword(unsigned char offset,unsigned char *buff);

void Open_Root_Directory(void);


void Check_MBR_FAT32(unsigned char *buff)
{

   unsigned char data=0;
   unsigned int ii=0;
   Read_Buffer(Master_Boot_Record,buff);
   for(ii=0;ii<15;ii++)
{

   if(txt_FAT32[ii]==buff[ii+0x49]){data=1;}else{data=0;}
   if(data==0){break;};
   }

   if(data==0){text3 ("FAT32 not Found",0 ,60,1,Black,White);    }
   if(data==1){text3 ("FAT32 Found",0 ,60,1,Black,White);    }

}








void Init_FAT32(void)
{


     unsigned int Data;

    //fillScreen2(Black);

    text3 ("FAT32 initialization",0 ,0,1,Black,White);

    text3 ("Read sector 0 ",0 ,20,1,Black,White);

     Read_Buffer(0,buffer);

     if(buffer[0x1c7]==0x20)
     {
      text3 ("MBR at 8192",0 ,40,1,Black,White);
     }

    Lineoffset=80;
    Check_MBR_FAT32(buffer);
     /**********************************************************/
    Bytes_Per_Sector=Reed_Word(0x0B,buffer);
    text3 ("Bytes_Per_Sector",0 ,Lineoffset,1,Black,White);
    intToStr(Bytes_Per_Sector,buff2,1,4);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;
    /**********************************************************/
    Sectors_Per_Cluster=Reed_Byte(0x0D,buffer);
    text3 ("Sectors_Per_Cluster",0 ,Lineoffset,1,Black,White);
    intToStr(Sectors_Per_Cluster,buff2,1,4);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;
     /**********************************************************/
    Number_of_FATs=Reed_Byte(0x10,buffer);
    text3 ("Number_of_FATs",0 ,Lineoffset,1,Black,White);
    intToStr(Number_of_FATs,buff2,1,4);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;
    /**********************************************************/

    Hidden_Sectors= Reed_Dword((0x1C),buffer);
    text3 ("Hidden_Sectors",0 ,Lineoffset,1,Black,White);
    intToStr(Hidden_Sectors,buff2,1,12);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;
    /**********************************************************/
    Sectors_Per_FAT= Reed_Dword((0x24),buffer);
    text3 ("Sectors_Per_FAT",0 ,Lineoffset,1,Black,White);
    intToStr(Sectors_Per_FAT,buff2,1,12);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;

   /**********************************************************/
    Root_Cluster_Number= Reed_Dword((0x2C),buffer);
    text3 ("Root_Cluster_Number",0 ,Lineoffset,1,Black,White);
    intToStr(Root_Cluster_Number,buff2,1,12);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;

    /**********************************************************/
    Reserved_Sectors= Reed_Word((0x0E),buffer);
    text3 ("Reserved_Sectors",0 ,Lineoffset,1,Black,White);
    intToStr(Reserved_Sectors,buff2,1,12);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;


    /**********************************************************/
    Data_Start_Sector=(Hidden_Sectors+Reserved_Sectors+((unsigned long)Number_of_FATs*Sectors_Per_FAT));
    Root_Directory=(Data_Start_Sector+((Root_Cluster_Number-2)*Sectors_Per_Cluster));

    if((Hidden_Sectors==0)&&(Master_Boot_Record!=0))
    {
        Data_Start_Sector=(Master_Boot_Record+Reserved_Sectors+((unsigned long)Number_of_FATs*Sectors_Per_FAT));
        Root_Directory=(Data_Start_Sector+((Root_Cluster_Number-2)*Sectors_Per_Cluster));
    }

    text3 ("Root_Directory",0 ,Lineoffset,1,Black,White);
    intToStr(Root_Directory,buff2,1,12);
    text3 (buff2,180 ,Lineoffset,1,Black,White);
    Lineoffset=Lineoffset+20;


    Open_Root_Directory();

}










  
void Read_Directory_Entry_Raw(unsigned long Directory_Entry, unsigned int RawOffset, unsigned char *entry_buff)
{
    unsigned int Buffer_Offset;
    unsigned int Offset;
    unsigned int ii;

    Buffer_Offset=(RawOffset/32);
    Offset=(RawOffset-(Buffer_Offset*32));

    Read_Buffer((Directory_Entry+Buffer_Offset),buffer);

    for(ii=0;ii<32;ii++)
    {
        entry_buff[ii]=buffer[(Offset*0x10)+ii];
    }
}

unsigned char Upper_Ascii(unsigned char data_in)
{
    if((data_in>=0x61)&&(data_in<=0x7A))
    {
        data_in=(data_in-0x20);
    }

    return data_in;
}

void Format_Short_Filename_Internal(unsigned char *entry_buff,char *txtbuff)
{
    unsigned int out_index;
    unsigned int ii;

    out_index=0;

    for(ii=0;ii<50;ii++)
    {
        txtbuff[ii]=0x00;
    }

    for(ii=0;ii<8;ii++)
    {
        if(entry_buff[ii]==0x20)
        {
            break;
        }

        if(out_index<49)
        {
            txtbuff[out_index]=entry_buff[ii];
            out_index++;
        }
    }

    if((entry_buff[8]!=0x20)||(entry_buff[9]!=0x20)||(entry_buff[10]!=0x20))
    {
        if(out_index<49)
        {
            txtbuff[out_index]='.';
            out_index++;
        }

        for(ii=8;ii<11;ii++)
        {
            if(entry_buff[ii]==0x20)
            {
                break;
            }

            if(out_index<49)
            {
                txtbuff[out_index]=entry_buff[ii];
                out_index++;
            }
        }
    }

    txtbuff[out_index]=0x00;
}

unsigned int Search_File_Indirectory(unsigned long Directory_Entry, unsigned char *buff,unsigned int *buff2)
{
    unsigned int sector_index;
    unsigned int entry_index;
    unsigned int counter;
    unsigned int raw_offset;
    unsigned char first_byte;
    unsigned char attribute;

    counter=0;
    sector_index=0;

    while(1)
    {
        Read_Buffer((Directory_Entry+sector_index),buff);

        for(entry_index=0;entry_index<16;entry_index++)
        {
            first_byte=buff[(entry_index*0x20)];
            attribute=buff[(entry_index*0x20)+0x0B];

            if(first_byte==0x00)
            {
                return counter;
            }

            if(first_byte==0xE5)
            {
                continue;
            }

            if(attribute==0x0F)
            {
                continue;
            }

            if((attribute&0x08)!=0)
            {
                continue;
            }

            if(first_byte==0x2E)
            {
                continue;
            }

            counter++;
            raw_offset=(sector_index*32)+(entry_index*2);
            buff2[counter]=raw_offset;

            if(counter>=899)
            {
                return counter;
            }
        }

        sector_index++;

        if(sector_index>=256)
        {
            break;
        }
    }

    return counter;
}

void Reed_Filename(unsigned long Directory_Entry, unsigned int Filenumber, char *txtbuff)
{
    unsigned char entry_buff[32];

    Read_Directory_Entry_Raw(Directory_Entry,File_Offset[Filenumber],entry_buff);
    Format_Short_Filename_Internal(entry_buff,txtbuff);
}

unsigned char Reed_File_Type(unsigned long Directory_Entry, unsigned int Filenumber )
{
    unsigned char entry_buff[32];
    unsigned char attribute;
    unsigned char Data;

    Data=0x03;

    Read_Directory_Entry_Raw(Directory_Entry,File_Offset[Filenumber],entry_buff);
    attribute=entry_buff[0x0B];

    if((attribute&0x10)!=0)
    {
        return 0x01;
    }

    if((Upper_Ascii(entry_buff[8])=='A')&&(Upper_Ascii(entry_buff[9])=='V')&&(Upper_Ascii(entry_buff[10])=='I'))
    {
        return 0x02;
    }

    return Data;
}

unsigned long Reed_File_Size(unsigned long Directory_Entry, unsigned int Filenumber )
{
    unsigned char entry_buff[32];
    unsigned long Data;
    unsigned long HIGHWORD;
    unsigned long LOWWORD;
    unsigned long HIGHBYTE;
    unsigned long LOWBYTE;

    Read_Directory_Entry_Raw(Directory_Entry,File_Offset[Filenumber],entry_buff);

    LOWBYTE= entry_buff[0x1C];
    HIGHBYTE= entry_buff[0x1D];
    LOWWORD=((HIGHBYTE<<8)|LOWBYTE);

    LOWBYTE= entry_buff[0x1E];
    HIGHBYTE= entry_buff[0x1F];
    HIGHWORD=((HIGHBYTE<<8)|LOWBYTE);

    Data=((HIGHWORD<<16)|LOWWORD);

    return Data;
}

unsigned char Reed_Long_Filename(unsigned long Directory_Entry, unsigned int Filenumber, char *txtbuff)
{
    unsigned char entry_buff[32];
    unsigned int raw_offset;
    unsigned int segment_base;
    unsigned int char_index;
    unsigned int out_index;
    unsigned int i;
    unsigned char ordinal;
    unsigned char found;
    unsigned char char_offsets[13]={0x01,0x03,0x05,0x07,0x09,0x0E,0x10,0x12,0x14,0x16,0x18,0x1C,0x1E};

    found=0;

    for(i=0;i<50;i++)
    {
        txtbuff[i]=0x00;
    }

    if(Filenumber==0)
    {
        return 0;
    }

    raw_offset=File_Offset[Filenumber];

    if(raw_offset<2)
    {
        return 0;
    }

    raw_offset=raw_offset-2;

    while(1)
    {
        Read_Directory_Entry_Raw(Directory_Entry,raw_offset,entry_buff);

        if(entry_buff[0x0B]!=0x0F)
        {
            break;
        }

        ordinal=(entry_buff[0]&0x1F);

        if(ordinal==0)
        {
            break;
        }

        segment_base=((unsigned int)(ordinal-1)*13);

        for(char_index=0;char_index<13;char_index++)
        {
            if(segment_base+char_index>=49)
            {
                break;
            }

            out_index=char_offsets[char_index];

            if(entry_buff[out_index]==0x00)
            {
                txtbuff[segment_base+char_index]=0x00;
                found=1;
                break;
            }

            if(entry_buff[out_index]==0xFF)
            {
                break;
            }

            txtbuff[segment_base+char_index]=entry_buff[out_index];
            found=1;
        }

        if(raw_offset<2)
        {
            break;
        }

        raw_offset=raw_offset-2;
    }

    return found;
}

unsigned long File_Start_Addres(unsigned long Directory_Entry,unsigned int Filenumber )
{
    unsigned char entry_buff[32];
    unsigned long CLUSTER;
    unsigned long Data;
    unsigned long HIGHWORD;
    unsigned long LOWWORD;
    unsigned long HIGHBYTE;
    unsigned long LOWBYTE;

    Read_Directory_Entry_Raw(Directory_Entry,File_Offset[Filenumber],entry_buff);

    LOWBYTE= entry_buff[0x1A];
    HIGHBYTE= entry_buff[0x1B];
    LOWWORD=((HIGHBYTE<<8)|LOWBYTE);

    LOWBYTE= entry_buff[0x14];
    HIGHBYTE= entry_buff[0x15];
    HIGHWORD=((HIGHBYTE<<8)|LOWBYTE);

    CLUSTER=((HIGHWORD<<16)|LOWWORD);

    if(CLUSTER<2)
    {
        Data=0;
    }
    else
    {
        Data=(Data_Start_Sector+((CLUSTER-2)*Sectors_Per_Cluster));
    }

    return Data;
}




void Open_Directory(unsigned long Directory_Entry)
{
 Read_Buffer(Directory_Entry, buffer);

}


void Open_Root_Directory(void)
{
 Read_Buffer(Root_Directory, buffer);

}




unsigned int Reed_Word(unsigned char offset,unsigned char *buff)
{

    unsigned int Data;
    unsigned int HIGHBYTE=0;
    unsigned char LOWBYTE=0;

    LOWBYTE=buff[offset];
    HIGHBYTE=buff[(offset+1)];
    Data=((HIGHBYTE<<8)|LOWBYTE);
    return Data;

}



unsigned char Reed_Byte(unsigned char offset,unsigned char *buff)
{

    unsigned char Data;
    Data=buff[offset];
    return Data;

}


unsigned long Reed_Dword(unsigned char offset,unsigned char *buff)
{
    unsigned long Data;

    unsigned long HIGHWORD;
    unsigned int LOWWORD;
    unsigned int HIGHBYTE=0;
    unsigned int LOWBYTE=0;



    LOWBYTE=buff[offset];
    HIGHBYTE=buff[(offset+1)];
    LOWWORD=((HIGHBYTE<<8)|LOWBYTE);


    LOWBYTE=buff[offset+2];
    HIGHBYTE=buff[(offset+3)];
    HIGHWORD=((HIGHBYTE<<8)|LOWBYTE);

    Data=((HIGHWORD<<16)|LOWWORD);

    return Data;

}

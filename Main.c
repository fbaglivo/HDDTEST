#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "sgio.h"
#include <sys/stat.h>




int check_WDC_drive(void);
int VSC_enable(void);
int VSC_disable(void);
int VSC_send_key(char, int);
int VSC_send_write_key(char);
int VSC_send_read_key(int);
int handleChecksum(char *, int);
void cleanup(void);
void show_version(void);
void show_usage(void);
int VSC_get_mod1sec (unsigned int* );
int VSC_read_mod1_data(unsigned char , int);
int VSC_read_allmod_data(unsigned char , int );
int myreadmodule(struct ata_tf ,unsigned short,unsigned short );

int prefer_ata12=0;
int verbose=0;

int vscenabled=0;
int force=0;
int undo=0;

char *device;

int fd=0;

FILE *fp;

#define progname "HDDTEST"
#define VERSION "1.0"

#define VSC_KEY_READ 0x01
#define VSC_KEY_WRITE 0x02
#define BYTES_PER_SECTOR 512
#define MOD32 0x32
#define MOD1 0x1



int check_WDC_drive() {
   static __u8 atabuffer[4+512];
   int i;

   if (verbose) {
      printf("Checking if this is a Western Digital Hard Disk\n");
   }

   memset(atabuffer, 0, sizeof(atabuffer));
   atabuffer[0] = ATA_OP_IDENTIFY;
   atabuffer[3] = 1;
   if (do_drive_cmd(fd, atabuffer)) {
      perror("HDIO_DRIVE_CMD(identify) failed");
      return errno;
   } 

   if (!force) {
      /* Check for a Western Digital Hard Disk - first 3 characters : WDC*/
      if ( (atabuffer[4+(27*2)+1] != 'W') || (atabuffer[4+(27*2)] != 'D') || (atabuffer[4+(28*2)+1] != 'C')) {
         fprintf(stderr, "The device %s does not seem to be a Western Digital Hard Disk ", device);
         fprintf(stderr, "but a(n) ");
         for (i=27; i<47; i++) {
            if(atabuffer[4+(i*2)+1]==0)
               break;
            putchar(atabuffer[4+(i*2)+1]);
            if(atabuffer[4+(i*2)+0]==0)
               break;
            putchar(atabuffer[4+(i*2)+0]);
      }
      printf("\n\n");

      fprintf(stderr, "Use the --force option if you know what you're doing\n\n");
      return 1;
      }
   }
   return 0;
}

/* Enable Vendor Specific Commands */
int VSC_enable() {
   int err=0;
   struct ata_tf tf;

   if (verbose) {
      printf("Enabling Vendor Specific ATA commands\n");
   }

   tf_init(&tf, ATA_OP_VENDOR_SPECIFIC, 0, 0);
   tf.lob.feat = 0x45;
   tf.lob.lbam = 0x44;
   tf.lob.lbah = 0x57;
   tf.dev = 0xa0;

   if(sg16(fd, SG_WRITE, SG_PIO, &tf, NULL, 0, 5)) {
      err = errno;
      perror("sg16(VSC_ENABLE) failed");
      return err;
   }
   vscenabled=1;
   return 0;
}

/* Disable Vendor Specific Commands */
int VSC_disable() {
   int err=0;
   struct ata_tf tf;

   if (verbose) {
      printf("Disabling Vendor Specific ATA commands\n");
   }

   tf_init(&tf, ATA_OP_VENDOR_SPECIFIC, 0, 0);
   tf.lob.feat = 0x44;
   tf.lob.lbam = 0x44;
   tf.lob.lbah = 0x57;
   tf.dev = 0xa0;

   if(sg16(fd, SG_WRITE, SG_PIO, &tf, NULL, 0, 5)) {
      err = errno;
      perror("sg16(VSC_DISABLE) failed");
      return err;
   }
   vscenabled=0;
   return 0;
}

/* Vendor Specific Commands sendkey for Mod 32h */
int VSC_send_key(char rw, int nmod) {
   int err=0;
   char buffer[512];
   struct ata_tf tf;
   char lnmod=0, hnmod=0;
   unsigned short temp=0;

   tf_init(&tf, ATA_OP_SMART, 0, 0);
   tf.lob.feat = 0xd6;
   tf.lob.nsect = 0x01;
   tf.lob.lbal = 0xbe;
   tf.lob.lbam = 0x4f;
   tf.lob.lbah = 0xc2;
   tf.dev = 0xa0;

   printf("modnum : %x\n", nmod);

   lnmod=(char)nmod;
   temp=nmod>>8;
   hnmod=(char)temp;

   memset(buffer,0,sizeof(buffer));
   buffer[0]=0x08;
   buffer[2]=rw;
   buffer[5]=hnmod;
   buffer[4]=lnmod;

   printf("\n\nlow: %x high: %x temp: %x \n\n",lnmod,hnmod,temp);


   if(sg16(fd, SG_WRITE, SG_PIO, &tf, buffer, 512, 5)) {
      err = errno;
      perror("sg16(VSC_SENDKEY) failed");
      return err;
   }
   vscenabled=0;
   return 0;
}

int VSC_send_write_key(char modnum) {
   if (verbose)
      printf("Sending WRITE key\n");
   return VSC_send_key(VSC_KEY_WRITE, modnum);
}

int VSC_send_read_key(int modnum) {
   if (verbose)
      printf("Sending READ key\n");
   return VSC_send_key(VSC_KEY_READ, modnum);
}

int handleChecksum(char *buffer, int mod32len) {
   int i=0;
   long sum=0;
   int checksum;

   while (i<mod32len) {
      sum=sum + ((unsigned char)(buffer[i]) |
                ((unsigned char)buffer[i+1]<<8) |
                ((unsigned char)buffer[i+2]<<16) |
                ((unsigned char)buffer[i+3]<<24));
      i+=4;
   }
   checksum=0xFFFFFFFF-sum+1;

//Do nothing if 0xC-0xF has the correct checksum
   if (sum==0)
      return 0;

//Calculate the new checksum
   else if ((*(buffer+12)==0) && (*(buffer+13)==0) && (*(buffer+14)==0) && (*(buffer+15)==0)) {
      *(buffer+12)=checksum & 0xFF;
      *(buffer+13)=checksum>>8 & 0xFF;
      *(buffer+14)=checksum>>16 & 0xFF;
      *(buffer+15)=checksum>>24 & 0xFF;

      return 0;
   }

//Exit if checksum in patched Mod 32h file is invalid
   else
      return 1;
}

/* Called on exit, last chance do call VSC_disable if needed */
void cleanup(void) {
   if (fd==0)
      return;
   if (vscenabled != 0)
      VSC_disable();
   close(fd);
}

/* Version */
void show_version(void) {
   printf("%s v%s\n", progname, VERSION);
}

/* Usage */
void show_usage(void) {
   printf("%s v%s - Patch Mod 32h of Western Digital Hard Disks for data recovery purposes\n", progname, VERSION);
   printf("Copyright (C) 2015 KHONG How Yu\n");
   printf("For Franc Zabkar\n");
   printf("Based on idle3ctl 0.9.1 by Christophe Bothamy\n");
   printf("\nThis an independent software, unrelated in any way to Western Digital Corp. or Christophe Bothamy.\n\n");
   printf("WARNING : THIS SOFTWARE IS EXPERIMENTAL AND NOT WELL TESTED. USE AT YOUR OWN RISK!\n");
   printf("\n");
   printf("Usage: mod32patch [options] device\n");
   printf("Options: \n");
   printf(" -r        : dump Mod 32h into a mod32ori.bin file and generate mod32new.bin patch file\n");
   printf(" -h        : display help\n");
   printf(" -u        : undo the patch by restoring the original mod32ori.bin into your Western Digital HDD\n");
   printf(" -v        : verbose output\n");
   printf(" -V        : show version and exit immediately\n");
   printf(" -w        : write the patched Mod 32h into the Western Digital HDD\n");
   printf(" --force   : force even if no Western Digital HDD is detected\n\n");
}

/* Vendor Specific Commands get Mod 1 number of sectors */

int VSC_get_mod1sec (unsigned int *sectors) {
   int err=0;
   char buffer[512];
   struct ata_tf tf;

   if (verbose)
      printf("Getting number of sectors in Mod 01h\n");

   tf_init(&tf, ATA_OP_SMART, 0, 0);
   tf.lob.feat = 0xd5;
   tf.lob.nsect = 0x01;
   tf.lob.lbal = 0xbf;
   tf.lob.lbam = 0x4f;
   tf.lob.lbah = 0xc2;
   tf.dev = 0xa0;

   memset(buffer,0,sizeof(buffer));

   if(sg16(fd, SG_READ, SG_PIO, &tf, buffer, 512, 5)) {
      err = errno;
      perror("sg16(VSC_GET_MOD32) failed");
      return err;
   }

   *sectors=buffer[10] | buffer[11]<<8;
   printf("Number of sectors in Mod 01h : 0x%x\n", *sectors);
   return 0;
}

int VSC_read_mod1_data(unsigned char mod1sec, int mod1len) {

   int err=0;
   int t=0;
   int offset=0x32;
   int total_records=0;
   int register_lenght=0;
   unsigned char module_id_l=0;
   unsigned char module_id_h=0;
   unsigned char module_size_l=0;
   unsigned char module_init_l=0;
   unsigned char module_size_h=0;
   unsigned char module_init_h=0;

   unsigned short   module_id=0;
   unsigned short   module_size=0;
   unsigned short   module_init=0;


   char buffer[mod1len];
   struct ata_tf tf;



   if (verbose)
      printf("Getting Mod1h contents\n");

   tf_init(&tf, ATA_OP_SMART, 0, 0);
   tf.lob.feat = 0xd5;
   tf.lob.nsect = mod1sec;
   tf.lob.lbal = 0xbf;
   tf.lob.lbam = 0x4f;
   tf.lob.lbah = 0xc2;
   tf.dev = 0xa0;

   memset(buffer,0,sizeof(buffer));

   if(sg16(fd, SG_READ, SG_PIO, &tf, buffer, mod1len, 5)) {
      err = errno;
      perror("sg16(VSC_GET_MOD32) failed");
      return err;
   }

   fp=fopen("mod1ori.bin", "wb");
   fwrite(buffer, 1, mod1len, fp);
   fclose(fp);

   total_records=buffer[0x31]<<8 | buffer[0x30]  ;
   printf("\n There are %d records \n ", total_records);
   register_lenght=buffer[offset];
   printf("\n Records Lenght is: 0x%x \n ", register_lenght);

   printf("\n ID     SIZE     INIT \n ");

   for (t=1;t<total_records;t++){


	   module_id_l=buffer[offset+2+(t-1)*register_lenght];
	   module_size_l=buffer[offset+4+(t-1)*register_lenght];
	   module_init_l= buffer[offset+10+(t-1)*register_lenght];
	   module_id_h=buffer[offset+3+(t-1)*register_lenght];
	   module_size_h=buffer[offset+5+(t-1)*register_lenght];
	   module_init_h= buffer[offset+11+(t-1)*register_lenght];


	   module_id= (module_id_h << 8 ) | (module_id_l);
	   module_size= (module_size_h << 8 ) | (module_size_l);
	   module_init= (module_init_h << 8 ) | (module_init_l);


	   printf("\n 0x%x    %d    %d      \n ", module_id, module_size, module_init);

   }

   return 0;
}


int VSC_read_allmod_data(unsigned char mod1sec, int mod1len) {

   int err=0;
   int t=0;
   int offset=0x32;
   int total_records=0;
   int register_lenght=0;
   unsigned char module_id_l=0;
   unsigned char module_id_h=0;
   unsigned char module_size_l=0;
   unsigned char module_init_l=0;
   unsigned char module_size_h=0;
   unsigned char module_init_h=0;

   unsigned short   module_id=0;
   unsigned short   module_size=0;
   unsigned short   module_init=0;
   unsigned short   real_module_size=0;



   char buffer[mod1len];
   struct ata_tf tf;



   if (verbose)
      printf("Getting Mod1h contents\n");

   tf_init(&tf, ATA_OP_SMART, 0, 0);
   tf.lob.feat = 0xd5;
   tf.lob.nsect = mod1sec;
   tf.lob.lbal = 0xbf;
   tf.lob.lbam = 0x4f;
   tf.lob.lbah = 0xc2;
   tf.dev = 0xa0;

   memset(buffer,0,sizeof(buffer));

   if(sg16(fd, SG_READ, SG_PIO, &tf, buffer, mod1len, 5)) {
      err = errno;
      perror("sg16(VSC_GET_MOD32) failed");
      return err;
   }


   total_records=buffer[0x31]<<8 | buffer[0x30]  ;
   register_lenght=buffer[offset];

   for (t=1;t<total_records;t++){


	   module_id_l=buffer[offset+2+(t-1)*register_lenght];
	   module_size_l=buffer[offset+4+(t-1)*register_lenght];
	   module_init_l= buffer[offset+10+(t-1)*register_lenght];
	   module_id_h=buffer[offset+3+(t-1)*register_lenght];
	   module_size_h=buffer[offset+5+(t-1)*register_lenght];
	   module_init_h= buffer[offset+11+(t-1)*register_lenght];


	   module_id= (module_id_h << 8 ) | (module_id_l);
	   module_size= (module_size_h << 8 ) | (module_size_l);
	   module_init= (module_init_h << 8 ) | (module_init_l);


	   printf("\n %d    %d    %d      \n ", module_id, module_size, module_init);
	   // CHEQUEAR COMO HACER EL BUFFER DE TAMAÑO VARIABLE

	   tf_init(&tf, ATA_OP_SMART, 0, 0);
	   tf.lob.feat = 0xd5;
	   tf.lob.nsect = module_size;
	   tf.lob.lbal = 0xbf;
	   tf.lob.lbam = 0x4f;
	   tf.lob.lbah = 0xc2;
	   tf.dev = 0xa0;

	 // if (module_id<0xFF)
	   //{
		   if (VSC_send_read_key((int)module_id)!=0)

			   exit(1);

		   real_module_size=BYTES_PER_SECTOR*module_size;
		   myreadmodule(tf,real_module_size,module_id);
	   	//}



   }

   return 0;
}


int myreadmodule(struct ata_tf tf,  unsigned short  module_size, unsigned short module_id)

{
	  char buf=10;
	  int err=0;
	  char buffer[module_size];

 	   if(sg16(fd, SG_READ, SG_PIO, &tf, buffer,(int)module_size, 5)) {
	 	        err = errno;
	 	        perror("xxxsg16(VSC_GET_) failed");
	 	        return err;
	 	     }

 	  char str[10];
 	  snprintf(str, 10, "%s%x%s","mod",module_id,".bin");
 	  fp=fopen(str, "wb");
 	  fwrite(buffer, sizeof(char),module_size, fp);
 	  fclose(fp);

 	   printf("\n OK");
 	   return 0;
}


int main(int argc, char **argv) {

       int i=1;
	   unsigned int mod1sec;
	   unsigned int mod1len=0;

	   int action=1;       /* 1: read raw, 2: write */
	   int display_usage=1;


	   for (i=1; i<argc; i++) {
	        if (strcmp(argv[i],"-h")==0) {
	           show_usage();
	           exit(1);
	        }
	        else if (strcmp(argv[i],"-V")==0) {
	           show_version();
	           exit(1);
	        }
	        else if (strcmp(argv[i],"--force")==0)
	           force=1;
	        else if (strcmp(argv[i],"-v")==0)
	           verbose=1;
	        else if (strcmp(argv[i],"-r")==0)
	           action=1;
	        else if (strcmp(argv[i],"-u")==0) {
	           action=2;
	           undo=1;
	        }
	        else if (strcmp(argv[i],"-w")==0) {
	           action=2;
	        }
	        else if (strncmp(argv[i],"-",1)==0) {
	          fprintf(stderr,"Unknown option '%s'\n\n",argv[i]);
	          show_usage();
	          exit(1);
	        }
	        else {

		       // Open device
	         device = argv[i];
	         fd = open(device, O_RDONLY|O_NONBLOCK);
	         if (fd<0) {
	            perror("open failed");
	            exit(1);
	         }

	         // Check if HDD is a WD
	         if (check_WDC_drive() != 0)
	            exit(1);

	         printf("\n It is a WD!!");

	         if (VSC_enable()!=0)
	            exit(1);

	         printf("\n VSC ENABLED!!");

	         // Read Module 1 : Dir

	         if (VSC_send_read_key(MOD1)!=0)
	                         exit(1);


	         if (VSC_get_mod1sec(&mod1sec)!=0)
	                         exit(1);

	         mod1len=BYTES_PER_SECTOR*mod1sec;
	         printf("mod1sec : %d\n", mod1sec);
	         printf("mod1len : %d\n", mod1len);

	         if (VSC_send_read_key(MOD1)!=0)
	                        exit(1);

	         if (VSC_read_mod1_data(mod1sec, mod1len)!=0)
	                         exit(1);

	                     if (mod1len==0)
	                        printf("MOD1 is 0\n\n");
	                     else
	                        printf("MOD1 length is %d (0x%x)\n\n", mod1len, mod1len);

            if (VSC_send_read_key(MOD1)!=0)
               	                        exit(1);


	         if (VSC_read_allmod_data(mod1sec, mod1len)!=0)
	           	                         exit(1);

	         VSC_disable();

	         printf("\n VSC DISABLED!!\n");

	         close(fd);
	         fd=0;
	        }
	   }
	         //exit(0);

}

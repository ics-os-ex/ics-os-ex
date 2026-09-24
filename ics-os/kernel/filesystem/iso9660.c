/*
  Name: iso9660.c
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 07/02/04 07:36
  Description: The module that handles the ISO9660 filesystem on most CD-ROMS
  
    DEX educational extensible operating system 1.0 Beta
    Copyright (C) 2004  Joseph Emmanuel DL Dayo

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
*/
   #include "../dextypes.h"
   #include "../vfs/vfs_core.h"
   #include "../devmgr/dex32_devmgr.h"
   #include "../iomgr/iosched.h"
   #include "../process/process.h"
   #include "iso9660.h"
   
   
   int iso9660_myid = 0;
int iso9660_loaddirectory(iso9660_directory *dirinfo, void **buffer,int id)
{
     int sectors;
     int handle;
     *buffer = malloc(dirinfo->length_le);
        //obtain the number of sectors
        if  ((dirinfo->length_le % 2048) != 0)
                sectors = (dirinfo->length_le / 2048) + 1;
          else
                sectors = (dirinfo->length_le / 2048);
        
        handle=dex32_requestIO(id,IO_READ,dirinfo->first_sector_le ,sectors,*buffer);
        while (!dex32_IOcomplete(handle))
           taskswitch();
        dex32_closeIO(handle);    
        
        return dirinfo->length_le;
   };
   
   /*This function is called when the VFS first mounts a drive to the filesystem*/
   int iso9660_mountroot(vfs_node *node,int id)
   {
      DWORD handle;
      /* Always read into a full 2048-byte sector buffer — never a smaller
         on-stack struct (PIO writes the whole ATAPI block). */
      static BYTE sector_buf[2048] __attribute__((aligned(16)));
      iso9660_volumedescriptor current_vd;
      int vol_num = 1;
      
      printf("reading primary volume descriptor..\n");
      handle=dex32_requestIO(id,IO_READ,16,1,sector_buf);
      while (!dex32_IOcomplete(handle))
         taskswitch();
      dex32_closeIO(handle);       
      memcpy(&current_vd , sector_buf, sizeof(iso9660_volumedescriptor));
      
      //Check for a joliet volume descriptor
      
      do {
      printf("reading secondary volume descriptor..\n");
      
      handle=dex32_requestIO(id,IO_READ,16+vol_num,1,sector_buf);
      while (!dex32_IOcomplete(handle))
         taskswitch();
      dex32_closeIO(handle);      
      
      if ( iso9660_isjoliet((iso9660_volumedescriptor*)sector_buf) )
        {
           printf("Joliet Extension SVD detected\n");     
           memcpy(&current_vd,sector_buf,sizeof(iso9660_volumedescriptor));
           break;     
        };
      if (sector_buf[0] == 3)
        {
           printf("Volume Partition Descriptor detected\n");      
        };    
      vol_num++;
      } while (sector_buf[0]<255&& (vol_num<8) );
      
      printf("Interpreting volume descriptor..\n");
      //validate data
      if ( current_vd.descriptors[0] == 67 &&
           current_vd.descriptors[1] == 68 &&
           current_vd.descriptors[2] == 48 &&
           current_vd.descriptors[3] == 48 &&
           current_vd.descriptors[4] == 49 &&
           current_vd.descriptors[5] == 1)
           {
                int i,size, type;
                void *buffer;
                printf("valid primary volume descriptor set detected.\n");
                printf("Bytes per lgical block %d\n", current_vd.sector_size_le);
                
                type  = iso9660_isjoliet(&current_vd);
                
                if (type)
                   printf("Joliet Extensions detected. Using UCS-2 Level %d character set.\n",type);
  
                //print the volume identifier
                for (i=0;i<32;i++)
                    printf("%c",current_vd.volume_ident[i]);
                printf("\n");     
                
                printf("mounting root directory\n");
                size = iso9660_loaddirectory( (iso9660_directory*) &current_vd.rootdirrec,
                                        &buffer, id);
                node->misc = 0;
                node->miscsize = 0;                                                                 
                node->misc2=(void*)buffer;
                node->miscsize2=size;
                node->fsid = iso9660_myid;
                node->memid = id;
                node->misc_flag = type;
                
                /*If the CD driver supports drive locking, the "$cd -lock command"
                  should lock the CD to prevent it from being removed*/
                devmgr_sendmessage(id, DEVMGR_MESSAGESTR, "$cd -lock");
                
                iso9660_mount(node,buffer,id);

           }
      else
           {
                printf("invalid primary volume descriptor!.\n");
                printf("  ident=%d sig=%c%c%c%c%c ver=%d\n",
                       (int)current_vd.ident,
                       current_vd.descriptors[0], current_vd.descriptors[1],
                       current_vd.descriptors[2], current_vd.descriptors[3],
                       current_vd.descriptors[4], (int)current_vd.descriptors[5]);
                return -1; //return with error, tell the VFS not to mount
           };
           
      return 1;
   };  
   

   char *iso9660_convertname(const char *identifier, char *targ, int length)
   {
      int i;
      if (length > 255) length = 255;
      for (i = 0; i < length; i++) {
         char c = identifier[i];
         if (c == ';') /* ISO version separator */
            break;
         if (c == 0)
            break;
         /* Present ISO9660 names in lowercase for Unix-style paths. */
         if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
         targ[i] = c;
      }
      targ[i] = 0;
      /* Strip trailing dots from "NAME." Rock Ridge-less ISO names */
      while (i > 0 && targ[i - 1] == '.') {
         i--;
         targ[i] = 0;
      }
      return targ;
   };
   
   /*unicode to ascii converter*/
    char *iso9660_iso_unicodetoascii(WORD *unicodestr,char *targ,int length)
    {
    int i;
    for (i=0;unicodestr[i]&&i<(length/2);i++)
    {
       char ascii = (char)( (unicodestr[i]&0xFF00) >> 8);
       if (ascii == ';') break;
       targ[i]=ascii;
    };
    targ[i]=0;
    return targ;
    };

    /*Rock Ridge (SUSP) support. Modern ISO9660 writers (xorriso/grub-mkrescue,
      mkisofs -R) store the true Unix filename in a system-use record even when
      the level-1 (8.3) name differs (e.g. "LDSCRIPT" vs "ldscripts"). Extract the
      NNM ("Network Name") long name from the record's system-use area.

      In this tree the directory record uses a 33-byte fixed header (file
      identifier at offset 33), so the system-use area begins right after the
      identifier, padded to the next even offset. Each SUSP descriptor is
      [signature(2)][len(1)][version(1)][data...]; the NNM data is
      [flags(1)][name...]. Returns 1 and fills targ when a long name is found. */
    int iso9660_rockname(const iso9660_directory *dir, char *targ, int tlen)
    {
       const unsigned char *rec = (const unsigned char *)dir;
       int reclen = dir->size;
       int susp = 33 + dir->ident_length;
       int dlen;

       if (tlen < 2) return 0;
       if (susp >= reclen) return 0;
       if (susp & 1) susp++;
       if (susp >= reclen) return 0;

       while (susp + 4 <= reclen)
       {
          dlen = rec[susp + 2];
          if (dlen < 5 || susp + dlen > reclen)
             return 0;
          if (rec[susp] == 'N' && rec[susp + 1] == 'M')
          {
             int nl = dlen - 5;
             int i;
             if (nl > tlen - 1) nl = tlen - 1;
             for (i = 0; i < nl && rec[susp + 5 + i] != 0; i++)
                targ[i] = (char)rec[susp + 5 + i];
             targ[i] = 0;
             return 1;
          }
          susp += dlen;
       }
       return 0;
    };

/*Gets bytes per block assuming mode 1*/   
int iso9660_getbytesperblock()
{
    return 2048;
};

int iso9660_openfile(vfs_node *f,char *buffer,int start,int end,int id)
{
    DWORD startblock, endblock;
    iso9660_directory *dir = f->misc;
    DWORD firstblock = dir->first_sector_le;
    enum { CHUNK_BLOCKS = 32 };                 /* 64 KiB per CD request */
    char *chunk = malloc(CHUNK_BLOCKS * 2048);
    DWORD b, cb;
    int destoff = 0;

    if (end < start)
        return 0;
    if (!chunk)
        return 0;

    /* Read the CD in bounded 64KiB chunks directly into the caller's buffer.
       The previous implementation malloc'd an intermediate buffer the size of
       the whole transfer (2048 * totalblocks); for large executables (e.g. the
       18MiB in-OS GCC cc1) that doubled the peak kernel-heap requirement and
       the oversized single DMA read left the destination zeroed. Bounded
       multi-block reads keep the transfer efficient while never allocating an
       unbounded intermediate buffer. */
    startblock = start / 2048;
    endblock   = end / 2048;

    for (b = startblock; b <= endblock; ) {
        DWORD n = endblock - b + 1;
        if (n > CHUNK_BLOCKS)
            n = CHUNK_BLOCKS;

        {
            DWORD handle = dex32_requestIO(id, IO_READ, firstblock + b, n, chunk);
            while (!dex32_IOcomplete(handle))
                taskswitch();
            dex32_closeIO(handle);
        }

        for (cb = 0; cb < n; cb++) {
            DWORD block = b + cb;
            int src_begin = (block == startblock) ? (int)(start % 2048) : 0;
            int src_end   = (block == endblock)   ? (int)(end % 2048)   : 2047;
            int copylen   = src_end - src_begin + 1;
            if (copylen <= 0)
                continue;
            memcpy(buffer + destoff, chunk + cb * 2048 + src_begin, copylen);
            destoff += copylen;
        }
        b += n;
    }
    free(chunk);
    return 1;
};

int iso9660_unmount(vfs_node *directory,int id)
{
    devmgr_sendmessage(directory->memid, DEVMGR_MESSAGESTR, "$cd -unlock");
};

int iso9660_mountdirectory(vfs_node *directory, int id)
{
     int dirsize;
     char *buffer;
     //load the directory information
     dirsize=iso9660_loaddirectory(( iso9660_directory*)directory->misc,&buffer,id);
     
     directory->misc2 = (void*)buffer;
     directory->miscsize2 = dirsize;
     directory->attb = FILE_DIRECTORY | FILE_OREAD;
     iso9660_mount(directory,buffer,id);
     
     return 1;
};


   /*determines if this CD-ROM uses joliet extensions which requires UNICODE
     translation*/
   int iso9660_isjoliet(iso9660_volumedescriptor *vol)
   {
      if (vol->escape_sequence[0] == 0x25 && 
          vol->escape_sequence[1] == 0x2F)
        {   
         if (vol->escape_sequence[2] == 0x40)
            //UCS-2 Level 1
            return 1;
         if (vol->escape_sequence[2] == 0x43)
            //UCS-2 Level 2
            return 2;
         if (vol->escape_sequence[2] == 0x45)
            //UCS-2 level 3
            return 3;
         };
         return 0;
   };

   void iso9660_mount(vfs_node *mountpoint,char *dirbuffer, int devid)
   {
      char *dirptr = dirbuffer;
      DWORD size = 0;
      
      printf("mounting.. directory size %d..\n", mountpoint->miscsize2);
      
      while (size < mountpoint->miscsize2)
      {
            int i;
            iso9660_directory *dir =(iso9660_directory*)dirptr;
            vfs_node *node;
            /* Directory records do not span sectors; unused bytes at the
               end of a sector are zero. Skip to the next sector instead of
               treating that padding as end-of-directory (which hid later
               files like tccpp.c on multi-sector dirs). */
            if (dir->size == 0 ) {
               DWORD into = size % 2048;
               DWORD skip;
               if (into == 0)
                  break;
               skip = 2048 - into;
               if (size + skip >= mountpoint->miscsize2)
                  break;
               size += skip;
               dirptr += skip;
               continue;
            }
            
            //allocate a new vfs_node
            node=(vfs_node*)malloc(sizeof(vfs_node));
            memset(node,0,sizeof(vfs_node));
            
            vfs_createnode(node,mountpoint);
            
            if (dir->ident_length == 1 && dir->ident[0] == 0)
            strcpy(node->name,".");
              else
            if (dir->ident_length == 1 && dir->ident[0] == 1)  
            strcpy(node->name,"..");
              else
            if (mountpoint->misc_flag)  //Joliet Extensions??
            iso9660_iso_unicodetoascii(dir->ident,node->name,dir->ident_length);
              else
            {
               /* Level-1 name first, then prefer the Rock Ridge NNM long
                  name when the record carries one (e.g. "LDSCRIPT" ->
                  "ldscripts"). */
               iso9660_convertname(dir->ident,node->name,dir->ident_length);
               iso9660_rockname(dir,node->name,sizeof(node->name));
            }
            
            node->memid = devid;
            node->fsid = iso9660_myid;
            node->misc = dirptr;
            node->miscsize = 0; 
            node->miscsize2 = 0;
            node->attb = FILE_OREAD;
            node->size = dir->length_le;
            node->misc_flag = mountpoint->misc_flag;
            
            if (dir->flags & ISO9660_ATTBDIRECTORY)
               {
                node->attb = FILE_DIRECTORY | FILE_OREAD;
                node->files = VFS_NOT_MOUNTED;
               };
            
            size += dir->size;
            dirptr = dirptr + dir->size;
            
      };
      
   };
   
   void iso9660_init()
   {
      devmgr_fs_desc myfs;
      memset(&myfs,0,sizeof(myfs));
      strcpy(myfs.hdr.name,"cdfs");
      strcpy(myfs.hdr.description,"DEX ISO 9660/Joliet CD-ROM filesystem");
      myfs.hdr.size = sizeof(myfs);
      myfs.hdr.type = DEVMGR_FS;
      myfs.mountroot = iso9660_mountroot;
      myfs.mountdirectory = iso9660_mountdirectory;
      myfs.readfile = iso9660_openfile;
      myfs.getbytesperblock = iso9660_getbytesperblock;
      myfs.unmount = iso9660_unmount;
      iso9660_myid = devmgr_register((devmgr_generic*)&myfs);
   };

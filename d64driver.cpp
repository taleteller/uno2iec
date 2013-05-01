//
// Title        : D64DRIVER
// Author       : Lars Pontoppidan
// Date         : Jan. 2007
// Version      : 0.5
// Target MCU   : AtMega32(L) at 8 MHz
//
// CREDITS:
// --------
// This D64DRIVER module is inspired from code in Jan Derogee's 1541-III project
// for PIC: http://jderogee.tripod.com/
// This code is a complete reimplementation though.
//
// DESCRIPTION:
// This module works on top of the native file system, providing access to files
// in a D64 disk image.
//
// DISCLAIMER:
// The author is in no way responsible for any problems or damage caused by
// using this code. Use at your own risk.
//
// LICENSE:
// This code is distributed under the GNU Public License
// which can be found at http://www.gnu.org/licenses/gpl.txt
//

//#include <string.h>
#include "d64driver.hpp"
#ifdef CONSOLE_DEBUG
#include <QDebug>
#endif


namespace {

#define D64_BLOCK_SIZE 256  // Actual block size
#define D64_BLOCK_DATA 254  // Data capacity of block

#define D64_FIRSTDIR_TRACK  18
#define D64_FIRSTDIR_SECTOR 1

#define D64_BAM_TRACK  18
#define D64_BAM_SECTOR 0

#define D64_BAM_DISKNAME_OFFSET 0x90

#define D64_IMAGE_SIZE 174848

typedef struct {
  uchar disk_name[16]; // disk name padded with A0
  uchar disk_id[5];    // disk id and dos type
} d64_diskinfo;

// Sectors p. track table
// sectors_track[3] is the number of sectors in track 4 (!)
//
const uchar sectorsPerTrack[40] = {
  21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
  19,19,19,19,19,19,19,
  18,18,18,18,18,18,
  17,17,17,17,17,17,17,17,17,17
};

const char pstr_filetypes[] = "DELSEQPRGUSRREL???";
const QString pstr_blocksfree("BLOCKS FREE.");
const QString pstr_d64error("ERROR: D64");

} // anonymous


D64::D64(const QString& fileName)
  :  m_hostFile(fileName), m_status(NOT_READY), m_currentTrack(0), m_currentSector(0), m_currentOffset(0),
    m_currentLinkTrack(0), m_currentLinkSector(0)
{
  if(!fileName.isEmpty())
    openHostFile(fileName);
} // dtor


D64::~D64()
{
  if(!m_hostFile.fileName().isEmpty() and m_hostFile.isOpen())
    m_hostFile.close();
} // dtor


bool D64::openHostFile(const QString& fileName)
{
  if(!m_hostFile.fileName().isEmpty() and m_hostFile.isOpen())
    m_hostFile.close();

  m_hostFile.setFileName(fileName);
  if(m_hostFile.open(QIODevice::ReadOnly)) {
    // Check if file is a valid disk image by the simple criteria that
    // file size is at least 174.848
    if(hostSize() >= D64_IMAGE_SIZE) {
      m_status = DISK_OK;
      return true;
    }
  }

  // yikes.
  m_status = NOT_READY;
  return false;
} // openHostFile


// This function sets the filepointer to third byte in a block.
//
// It also reads in link to next block, which is what the two first bytes
// contains
//
void D64::seekBlock(uchar track, uchar sector)
{
  ushort absSector;
  ulong absOffset;
  uchar i;

#ifdef CONSOLE_DEBUG
  qDebug() << "seekblock: " << track << " " << sector << " ";
#endif

  // Change 1 based track notion to 0 based
  track--;

  //   // Sanity check track value
  //   if (track > 39) {
  //     m_status = 0;
  //     return;
  //   }
  //
  //   // Sanity check sector value
  //   if (sector > pgm_read_byte_near(&(sectors_track[track]))) {
  //     m_status = 0;
  //     return;
  //   }

  // Calculate absolute sector number
  absSector = sector;
  for(i = 0; i < track; i++)
    absSector += sectorsPerTrack[i];

  // Calculate absolute file offset
  absOffset = absSector * 256;

#ifdef CONSOLE_DEBUG
  qDebug() << absOffset << endl;
#endif

  // Seek to that position if possible
  if(absOffset < static_cast<ulong>(hostSize())) {
    hostSeek(absOffset);

    // Read in link to next block
    m_currentLinkTrack = hostReadByte();
    m_currentLinkSector = hostReadByte();

    // We are done, update vars
    m_currentTrack = track;
    m_currentSector = sector;
    m_currentOffset = 2;
  }
  else // Track or sector value must have been invalid! Bad image
    m_status = 0;
} // seekBlock


bool D64::isEOF(void) const
{
  return !(m_status bitand DISK_OK) or !(m_status bitand FILE_OPEN)
      or (m_status bitand FILE_EOF);
} // isEOF


// This function reads a character and updates file position to next
//
char D64::fgetc(void)
{
  uchar ret = 0;

  // Check status
  if(!isEOF()) {
    ret = hostReadByte();

    if(255 == m_currentOffset) {
      // We need to go to a new block, end of file?
      if(m_currentLinkTrack not_eq 0) {
        // Seek the next block:
        seekBlock(m_currentLinkTrack, m_currentLinkSector);
      }
      else
        m_status or_eq FILE_EOF;

    }
    else
      m_currentOffset++;
  }

  return ret;
} // fgetc



bool D64::fclose(void)
{
  m_status and_eq DISK_OK;  // Clear all flags except disk ok

  return true;
} // fclose


bool D64::seekFirstDir(void)
{
  if(m_status bitand DISK_OK) {
    // Seek to first dir entry
    seekBlock(D64_FIRSTDIR_TRACK, D64_FIRSTDIR_SECTOR);

    // Set correct status
    m_status = (D64Status)(DISK_OK bitor DIR_OPEN);

    return true;
  }
  return false;
} // seekFirstDir


bool D64::getDirEntry(DirEntry& dir)
{
  uchar i, j;
  uchar* pEntry = reinterpret_cast<uchar*>(&dir);

  // Check if correct status
  if((m_status bitand DISK_OK) and (m_status bitand DIR_OPEN)
     and !(m_status bitand DIR_EOF)) {

    // OK, copy current dir to target
    for(i = 0; i < 30; i++) {
      pEntry[i] = hostReadByte();
      m_currentOffset++;
    }

    // Have we crossed a block boundry?
    if(0 == m_currentOffset) {
      // We need to go to a new block, end of directory chain?
      if(m_currentLinkTrack not_eq 0) {
        // Seek the next block:
        seekBlock(m_currentLinkTrack, m_currentLinkSector);
      }
      else
        m_status or_eq DIR_EOF;

    }
    else {
      // No boundry crossing, skip past two initial bytes of next dir
      i = hostReadByte();
      j = hostReadByte();
      m_currentOffset += 2;

      if(0 == i and 0xFF == j) {
        // No more dirs!
        m_status or_eq DIR_EOF;
      }
    }

    return true;
  }
  return false;
} // getDirEntry


uchar D64::hostReadByte(uint length)
{
  char theByte;
  qint64 numRead(m_hostFile.read(&theByte, length));
  if(numRead < length) // shouldn't happen?
    m_status = FILE_EOF;

  return theByte;
} // hostReadByte


bool D64::hostSeek(qint32 pos, bool relative)
{
  if(relative)
    pos += m_hostFile.pos();

  return m_hostFile.seek(pos);
} // hostSeek


D64::D64Status D64::status(void) const
{
  return static_cast<D64Status>(m_status);
}

// At the position seeked comes:
//   16 chars of disk name (padded with A0)
//   2 chars of A0
//   5 chars of disk type
//
// Get these bytes directly from FAT by readHostByte();
void D64::seekToDiskName(void)
{
  if(m_status bitand DISK_OK) {
    // Seek BAM block
    seekBlock(D64_BAM_TRACK, D64_BAM_SECTOR);

    // Seek to disk name (-2 because seek_block already skips two bytes)
    hostSeek(D64_BAM_DISKNAME_OFFSET - 2, true);

    // Status now is no file open as such
    m_status = DISK_OK;
  }
} // seekToDiskName


ushort D64::blocksFree(void)
{
  // Not implemented yet
  return 0;
}

// Opens a file. Filename * will open first file with PRG status
//
bool D64::fopen(const QString& fileName)
{
  DirEntry dir;
  bool found = false;
  uchar len;
  uchar i;

  len = fileName.length();
  if(len > sizeof(dir.name))
    len = sizeof(dir.name);

  seekFirstDir();

  while(!found and getDirEntry(dir)) {

    // Acceptable filetype?
    i = dir.type bitand TYPE_MASK;
    if(SEQ == i or PRG == i) {

      // Compare filename respecting * and ? wildcards
      found = true;
      for(i = 0; i < len and found; i++) {
        if('?' == fileName[i]) {
          // This character is ignored
        }
        else if('*' == fileName[i]) {
          // No need to check more chars
          break;
        }
        else
          found = fileName[i] == dir.name[i];
      }

      // If searched to end of filename, dir.file_name must end here also
      if(found and (i == len))
        if(len < 16)
          found = dir.name[i] == 0xA0;

    }
  }

  if(found) {
    // File found. Jump to block and set correct state
    seekBlock(dir.track, dir.sector);
    m_status = (D64Status)(DISK_OK bitor FILE_OPEN);
  }

  return found;
} // fopen


void D64::sendListing(ISendLine& cb)
{
  ushort s;
  uchar c,i;
  DirEntry dir;
  char buffer[31];

  if(!(m_status bitand DISK_OK)) {
    // We are not happy with the d64 file
    cb.send(0, pstr_d64error.length(), pstr_d64error.toAscii().data());
    return;
  }

  // Send line with disc name and stuff, 25 chars
  seekToDiskName();

  buffer[0] = 0x12;   // Invert face
  buffer[1] = 0x22;   // "

  for(i = 2; i < 25; i++) {
    c = hostReadByte();

    if(c == 0xA0) // Convert padding A0 to spaces
      c = 0x20;

    if(i == 18)   // Ending "
      c = 0x22;

    buffer[i] = c;
  }

  cb.send(0, 25, buffer);

  // Prepare buffer
  memset(buffer, ' ', 3);
  buffer[3] = 0x22;   // quotes

  // Now for the list entries
  seekFirstDir();

  while(getDirEntry(dir)) {
    // Determine if dir entry is valid:
    if(dir.track not_eq 0) {
      // A direntry always takes 32 bytes total = 27 chars
      // initialize pos 4 -> 29 to spaces
      memset(&(buffer[4]), ' ', 26);

      // Send filename until A0 or 16 chars
      for (i = 4; i < 20; i++) {
        c = dir.name[i - 4];
        if (c == 0xA0)
          break;  // Filename is no longer

        buffer[i] = c;
      }

      // Ending dbl quotes
      buffer[i] = 0x22;

      // Write filetype
      i = dir.type bitand TYPE_MASK;

      if(i > 5) i = 5;

      memcpy(&(buffer[22]), pstr_filetypes + 3 * i, 3);

      // Perhaps write locked symbol
      if(dir.type bitand FILE_LOCKED)
        buffer[25] = '<';

      // Perhaps write splat symbol
      if(!(dir.type bitand FILE_CLOSED))
        buffer[26] = '*';

      // Line number is filesize in blocks:
      s = dir.blocksLo + (dir.blocksHi << 8);

      // Send initial spaces according to file size
      if(s >= 1000)
        i = 3;
      else if(s >= 100)
        i = 2;
      else if(s >= 10)
        i = 1;
      else
        i = 0;

      cb.send(s, 27, &(buffer[i]));
    }
  }

  // Send line with 0 blocks free
  QString blkFree(QString(pstr_blocksfree) + QString(13, ' '));
  cb.send(0, blkFree.length(), blkFree.toAscii().data());
} // sendListing
/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


/*
 *  Original code from libcdvd by Hiryu & Sjeep (C) 2002
 *  Modified by Florin for PCSX2 emu
 *  Fixed CdRead by linuzappz
 */

#include "PrecompiledHeader.h"

#include "CDVDdirReader.h"
#include "AsyncFileReader.h"
#include <wx/fswatcher.h>

#include <cstring>
#include <array>

// https://wiki.osdev.org/ISO_9660
// Primary Volume Descriptor 
struct ISO_9660_PVD
{
	u8   m_TypeCode;
	u8   m_StandardIdentifier[5];
	u8   m_Version;
	u8   _unused1;
	u8   m_SystemIdentifier[32];
	u8   m_VolumeIdentifier[32];
	u8   _unsued2[8];
	s32  m_VolumeSpaceSize_LSB;
	s32  m_VolumeSpaceSize_MSB;
	u8   _unsued3[32];
	s16  m_VolumeSetSize_LSB;
	s16  m_VolumeSetSize_MSB;
	s16  m_VolumeSequenceNumber_LSB;
	s16  m_VolumeSequenceNumber_MSB;
	s16  m_LogicalBlockSize_LSB;
	s16  m_LogicalBlockSize_MSB;
	s32  m_PathTableSize_LSB;
	s32  m_PathTableSize_MSB;
	s32  m_LPathTableSector_LSB;
	s32  m_OptionalLPathTableSector_LSB;
	s32  m_MPathTableSector_MSB;
	s32  m_OptionalMPathTableSector_MSB;
	u8   m_RootDirectoryEntry[34];
	u8   m_VolumeSetIdentifier[128];
	u8   m_PublisherIdentifier[128];
	u8   m_DataPreparerIdentifier[128];
	u8   m_ApplicationIdentifier[128];
	u8   m_CopyrightFileIdentifier[37];
	u8   m_AbstractFileIdentifier[37];
	u8   m_BibliographicFileIdentifier[37];
	u8   m_CreationDateTime[17];
	u8   m_ModificationDateTime[17];
	u8   m_ExpirationDateTime[17];
	u8   m_EffectiveDateTime[17];
	u8   m_FileStructureVersion;
};

constexpr u16 ISO_BLOCK_SIZE = 2048;

// first two sectors of ISO emulation
static u8 ISO_header[ISO_BLOCK_SIZE * 2];

static wxFileSystemWatcher watcher;

static int pmode, cdtype;

static s32 layer1start = -1;
static bool layer1searched = false;

s16 swap_s16(s16 val)
{
	return (val << 8) | ((val >> 8) & 0xFF);
}

s32 swap_s32(s32 val)
{
	val = ((val << 8) & 0xFF00FF00) | ((val >> 8) & 0xFF00FF);
	return (val << 16) | ((val >> 16) & 0xFFFF);
}

s16 as_little(s16 val)
{
	s16 test = 1;
	if (((u8*)&test)[0] == 0)
	{
		return swap_s16(val);
	}
	return val;
}
s16 as_big(s16 val)
{
	s16 test = 1;
	if (((u8*)&test)[0] == 1)
	{
		return swap_s16(val);
	}
	return val;
}

s32 as_little(s32 val)
{
	s32 test = 1;
	if (((u8*)&test)[0] == 0)
	{
		return swap_s32(val);
	}
	return val;
}
s32 as_big(s32 val)
{
	s32 test = 1;
	if (((u8*)&test)[0] == 1)
	{
		return swap_s32(val);
	}
	return val;
}

void fill_str(u8* dst, u16 dstSize, const char* str, u8 paddingChar=0x20)
{
	size_t len = strlen(str);
	memcpy(dst, str, len);
	memset(dst + len, paddingChar, dstSize - len);
}

void CALLBACK DIRclose()
{
	watcher.RemoveAll();
}

s32 CALLBACK DIRopen(const char* pPath)
{
	DIRclose(); // just in case

	if ((pPath == NULL) || (pPath[0] == 0))
	{
		Console.Error("CDVDdir Error: No path specified.");
		return -1;
	}

	wxString path = pPath;	
	if (!wxDir::Exists(path))
	{
		Console.Error("CDVDdir Error: Directory '%s' does not exist!", pPath);
		DIRclose();
		return -1;
	}

	if (!watcher.Add(path))
	{
		Console.Error("CDVDdir Error: Failed to watch directory path '%s'!", pPath);
		DIRclose();
		return -1;
	}

	wxArrayString files;
	size_t numFiles = wxDir::GetAllFiles(path, &files);
	if (numFiles == 0)
	{
		Console.Error("CDVDdir Error: Directory '%s' doesn't contain any files!", pPath);
		DIRclose();
		return -1;
	}

	memset(ISO_header, 0, sizeof(ISO_header));
	ISO_9660_PVD* pvd = (ISO_9660_PVD*)&ISO_header[0];

	pvd->m_TypeCode = 1;
	memcpy(pvd->m_StandardIdentifier, "CD001", 5);
	pvd->m_Version = 1;

	fill_str(pvd->m_SystemIdentifier, 32, "PLAYSTATION");
	fill_str(pvd->m_VolumeIdentifier, 32, "1");

	// number of blocks (sectors)
	pvd->m_VolumeSpaceSize_LSB = as_little((s32)4096);
	pvd->m_VolumeSpaceSize_MSB = as_big((s32)4096);

	// number of discs
	pvd->m_VolumeSetSize_LSB = as_little((s16)1);
	pvd->m_VolumeSetSize_MSB = as_big((s16)1);

	// number of this disc
	pvd->m_VolumeSequenceNumber_LSB = as_little((s16)1);
	pvd->m_VolumeSequenceNumber_MSB = as_big((s16)1);

	// block (sector) size
	pvd->m_LogicalBlockSize_LSB = as_little((s16)2048);
	pvd->m_LogicalBlockSize_MSB = as_big((s16)2048);


	pvd->m_PathTableSize_LSB;
	pvd->m_PathTableSize_MSB;
	pvd->m_LPathTableSector_LSB;
	pvd->m_OptionalLPathTableSector_LSB;
	pvd->m_MPathTableSector_MSB;
	pvd->m_OptionalMPathTableSector_MSB;
	pvd->m_RootDirectoryEntry[34];
	pvd->m_VolumeSetIdentifier[128];
	pvd->m_PublisherIdentifier[128];
	pvd->m_DataPreparerIdentifier[128];
	pvd->m_ApplicationIdentifier[128];
	pvd->m_CopyrightFileIdentifier[37];
	pvd->m_AbstractFileIdentifier[37];
	pvd->m_BibliographicFileIdentifier[37];
	pvd->m_CreationDateTime[17];
	pvd->m_ModificationDateTime[17];
	pvd->m_ExpirationDateTime[17];
	pvd->m_EffectiveDateTime[17];
	pvd->m_FileStructureVersion;

	for (size_t i = 0; i < numFiles; ++i)
	{
		wxString file = files[i];
		Console.WriteLn("Found File: %s", file.c_str().AsChar());
	}


	cdtype = CDVD_TYPE_PS2DVD;
	layer1start = -1;
	layer1searched = false;

	return 0;
}

s32 CALLBACK DIRreadSubQ(u32 lsn, cdvdSubQ* subq)
{
	// fake it
	u8 min, sec, frm;
	subq->ctrl = 4;
	subq->mode = 1;
	subq->trackNum = itob(1);
	subq->trackIndex = itob(1);

	lba_to_msf(lsn, &min, &sec, &frm);
	subq->trackM = itob(min);
	subq->trackS = itob(sec);
	subq->trackF = itob(frm);

	subq->pad = 0;

	lba_to_msf(lsn + (2 * 75), &min, &sec, &frm);
	subq->discM = itob(min);
	subq->discS = itob(sec);
	subq->discF = itob(frm);

	return 0;
}

s32 CALLBACK DIRgetTN(cdvdTN* Buffer)
{
	Buffer->strack = 1;
	Buffer->etrack = 1;

	return 0;
}

s32 CALLBACK DIRgetTD(u8 Track, cdvdTD* Buffer)
{
	if (Track == 0)
	{
		//Buffer->lsn = iso.GetBlockCount();
	}
	else
	{
		Buffer->type = CDVD_MODE1_TRACK;
		Buffer->lsn = 0;
	}

	return 0;
}

static bool testForPrimaryVolumeDescriptor(const std::array<u8, CD_FRAMESIZE_RAW>& buffer)
{
	const std::array<u8, 6> identifier = {1, 'C', 'D', '0', '0', '1'};
	return false;
	//return std::equal(identifier.begin(), identifier.end(), buffer.begin() + iso.GetBlockOffset());
}

static void FindLayer1Start()
{
	if (layer1searched)
		return;

	layer1searched = true;

	std::array<u8, CD_FRAMESIZE_RAW> buffer;

	// The ISO9660 primary volume descriptor for layer 0 is located at sector 16
	//iso.ReadSync(buffer.data(), 16);
	if (!testForPrimaryVolumeDescriptor(buffer))
	{
		Console.Error("isoFile: Invalid layer0 Primary Volume Descriptor");
		return;
	}

	// The volume space size (sector count) is located at bytes 80-87 - 80-83
	// is the little endian size, 84-87 is the big endian size.
	const int offset = 0;//iso.GetBlockOffset();
	uint blockresult = buffer[offset + 80] + (buffer[offset + 81] << 8) + (buffer[offset + 82] << 16) + (buffer[offset + 83] << 24);

	// If the ISO sector count is larger than the volume size, then we should
	// have a dual layer DVD. Layer 1 is on a different volume.
	if (blockresult < 0)//iso.GetBlockCount())
	{
		// The layer 1 start LSN contains the primary volume descriptor for layer 1.
		// The check might be a bit unnecessary though.
		//if (iso.ReadSync(buffer.data(), blockresult) == -1)
			return;

		if (!testForPrimaryVolumeDescriptor(buffer))
		{
			Console.Error("isoFile: Invalid layer1 Primary Volume Descriptor");
			return;
		}
		layer1start = blockresult;
		Console.WriteLn(Color_Blue, "isoFile: second layer found at sector 0x%08x", layer1start);
	}
}

// Should return 0 if no error occurred, or -1 if layer detection FAILED.
s32 CALLBACK DIRgetDualInfo(s32* dualType, u32* _layer1start)
{
	FindLayer1Start();

	if (layer1start < 0)
	{
		*dualType = 0;
		*_layer1start = 0;//iso.GetBlockCount();
	}
	else
	{
		*dualType = 1;
		*_layer1start = layer1start;
	}
	return 0;
}

s32 CALLBACK DIRgetDiskType()
{
	return cdtype;
}

s32 CALLBACK DIRgetTOC(void* toc)
{
	u8 type = DIRgetDiskType();
	u8* tocBuff = (u8*)toc;

	//CDVD_LOG("CDVDgetTOC\n");

	if (type == CDVD_TYPE_DVDV || type == CDVD_TYPE_PS2DVD)
	{
		// get dvd structure format
		// scsi command 0x43
		memset(tocBuff, 0, 2048);

		FindLayer1Start();

		if (layer1start < 0)
		{
			// fake it
			tocBuff[0] = 0x04;
			tocBuff[1] = 0x02;
			tocBuff[2] = 0xF2;
			tocBuff[3] = 0x00;
			tocBuff[4] = 0x86;
			tocBuff[5] = 0x72;

			tocBuff[16] = 0x00;
			tocBuff[17] = 0x03;
			tocBuff[18] = 0x00;
			tocBuff[19] = 0x00;
			return 0;
		}
		else
		{
			// dual sided
			tocBuff[0] = 0x24;
			tocBuff[1] = 0x02;
			tocBuff[2] = 0xF2;
			tocBuff[3] = 0x00;
			tocBuff[4] = 0x41;
			tocBuff[5] = 0x95;

			tocBuff[14] = 0x60; // dual sided, ptp

			tocBuff[16] = 0x00;
			tocBuff[17] = 0x03;
			tocBuff[18] = 0x00;
			tocBuff[19] = 0x00;

			s32 l1s = layer1start + 0x30000 - 1;
			tocBuff[20] = (l1s >> 24);
			tocBuff[21] = (l1s >> 16) & 0xff;
			tocBuff[22] = (l1s >> 8) & 0xff;
			tocBuff[23] = (l1s >> 0) & 0xff;
		}
	}
	else if ((type == CDVD_TYPE_CDDA) || (type == CDVD_TYPE_PS2CDDA) ||
			 (type == CDVD_TYPE_PS2CD) || (type == CDVD_TYPE_PSCDDA) || (type == CDVD_TYPE_PSCD))
	{
		// cd toc
		// (could be replaced by 1 command that reads the full toc)
		u8 min, sec, frm;
		s32 i, err;
		cdvdTN diskInfo;
		cdvdTD trackInfo;
		memset(tocBuff, 0, 1024);
		if (DIRgetTN(&diskInfo) == -1)
		{
			diskInfo.etrack = 0;
			diskInfo.strack = 1;
		}
		if (DIRgetTD(0, &trackInfo) == -1)
			trackInfo.lsn = 0;

		tocBuff[0] = 0x41;
		tocBuff[1] = 0x00;

		//Number of FirstTrack
		tocBuff[2] = 0xA0;
		tocBuff[7] = itob(diskInfo.strack);

		//Number of LastTrack
		tocBuff[12] = 0xA1;
		tocBuff[17] = itob(diskInfo.etrack);

		//DiskLength
		lba_to_msf(trackInfo.lsn, &min, &sec, &frm);
		tocBuff[22] = 0xA2;
		tocBuff[27] = itob(min);
		tocBuff[28] = itob(sec);

		for (i = diskInfo.strack; i <= diskInfo.etrack; i++)
		{
			err = DIRgetTD(i, &trackInfo);
			lba_to_msf(trackInfo.lsn, &min, &sec, &frm);
			tocBuff[i * 10 + 30] = trackInfo.type;
			tocBuff[i * 10 + 32] = err == -1 ? 0 : itob(i); //number
			tocBuff[i * 10 + 37] = itob(min);
			tocBuff[i * 10 + 38] = itob(sec);
			tocBuff[i * 10 + 39] = itob(frm);
		}
	}
	else
		return -1;

	return 0;
}

s32 CALLBACK DIRreadSector(u8* tempbuffer, u32 lsn, int mode)
{
	static u8 cdbuffer[CD_FRAMESIZE_RAW] = {0};

	int _lsn = lsn;

	//if (_lsn < 0)
	//	lsn = iso.GetBlockCount() + _lsn;
	//if (lsn >= iso.GetBlockCount())
	//	return -1;

	if (mode == CDVD_MODE_2352)
	{
		//iso.ReadSync(tempbuffer, lsn);
		return 0;
	}

	//iso.ReadSync(cdbuffer, lsn);


	u8* pbuffer = cdbuffer;
	int psize = 0;

	switch (mode)
	{
			//case CDVD_MODE_2352:
			// Unreachable due to shortcut above.
			//	pxAssume(false);
			//	break;

		case CDVD_MODE_2340:
			pbuffer += 12;
			psize = 2340;
			break;
		case CDVD_MODE_2328:
			pbuffer += 24;
			psize = 2328;
			break;
		case CDVD_MODE_2048:
			pbuffer += 24;
			psize = 2048;
			break;

			jNO_DEFAULT
	}

	memcpy(tempbuffer, pbuffer, psize);

	return 0;
}

s32 CALLBACK DIRreadTrack(u32 lsn, int mode)
{
	int _lsn = lsn;

	//if (_lsn < 0)
	//	lsn = iso.GetBlockCount() + _lsn;

	//iso.BeginRead2(lsn);

	pmode = mode;

	return 0;
}

s32 CALLBACK DIRgetBuffer(u8* buffer)
{
	return 0;
	//return iso.FinishRead3(buffer, pmode);
}

//u8* CALLBACK ISOgetBuffer()
//{
//	iso.FinishRead();
//	return pbuffer;
//}

s32 CALLBACK DIRgetTrayStatus()
{
	return CDVD_TRAY_CLOSE;
}

s32 CALLBACK DIRctrlTrayOpen()
{
	return 0;
}
s32 CALLBACK DIRctrlTrayClose()
{
	return 0;
}

s32 CALLBACK DIRdummyS32()
{
	return 0;
}

void CALLBACK DIRnewDiskCB(void (*/* callback */)())
{
}

CDVD_API CDVDapi_Folder =
	{
		DIRclose,

		DIRopen,
		DIRreadTrack,
		DIRgetBuffer,
		DIRreadSubQ,
		DIRgetTN,
		DIRgetTD,
		DIRgetTOC,
		DIRgetDiskType,
		DIRdummyS32, // trayStatus
		DIRdummyS32, // trayOpen
		DIRdummyS32, // trayClose

		DIRnewDiskCB,

		DIRreadSector,
		DIRgetDualInfo,
};

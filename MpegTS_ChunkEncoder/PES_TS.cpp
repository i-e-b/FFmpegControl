
#include "stdafx.h"
#include "MpegTS_ChunkEncoder.h"

#include <iostream>
#include <fstream>


Pests::Pests() {
}

void Pests::CloseFile() {
	if (!PushingStream || !PushingStream.is_open()) return;

	PushingStream.flush();
	PushingStream.flush();
	PushingStream.close();
}

void Pests::StartFile(char *FilePath, int *TrackIds, uint8_t *TrackTypes, int TrackCount) {
	if (PushingStream && PushingStream.is_open()) this->CloseFile();

	PushingStream.open(FilePath, std::ios::out | std::ios::binary);

	vCont = aCont = 0;

	WriteServicePacket();
	PushingStream.flush();
	WritePAT();
	PushingStream.flush();
	WritePMT(TrackIds, TrackTypes, TrackCount);
	PushingStream.flush();
}

void Pests::WriteServicePacket() {
	if (!PushingStream || !PushingStream.is_open()) return;
	uint8_t pad[188] = {0x47, 0x40, 0x11, 0x10, 0x00, 0x42, 0xB0, 0x25, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x01, 0xFC, 0x80, 0x14, 0x48, 0x12, 0x01, 0x06,
		0x50, 0x45, 0x53, 0x54, 0x53, 0x20, 0x49, 0x61, 0x69, 0x6E, 0x20, 0x42, 0x61, 0x6C, 0x6C, 0x61, 0x72, 0x64, 0x20, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	PushingStream.write((char*)pad, 188);
}

int Pests::CanPush() {
	if (!PushingStream || !PushingStream.is_open()) return 0;
	else return -1;
}

void Pests::WritePAT() {
	if (!PushingStream || !PushingStream.is_open()) return;

	int pkt_remaining = 188;
	pkt_remaining -= this->WriteTransportHeader(0x00, true, false); // PID of 0x0000 is PAT.

	uint8_t PAT_head[9] = {
		0x00,	// Pointer
		0x00,	// Table ID
		0xB0, 0x0D,	// Reserved + section length (= 13)
		0x00, 0x01, // transport ID (private)
		0xC1, 0x00, 0x00 // misc version control
	};

	PushingStream.write((char*)PAT_head, 9);
	pkt_remaining -= 9; // 13 bytes used, 175 bytes left
	
	// The PAT item is weird.
	// It specifies the PIDs of all PMTs and nothing else.
	uint8_t PAT_item[4] = {
		0x00, 0x01,	// PMT number (starts at 1 and counts up)
		0xEF, 0xFF	// Three bits '111', then the PID. (which is 0x0FFF)
	}; 
	PushingStream.write((char*)PAT_item, 4);
	pkt_remaining -= 4; // 18 bytes used, 171 bytes left

	uint8_t CRC[4] = {0x00, 0x00, 0x00, 0x00};
	PushingStream.write((char*)CRC, 4);
	pkt_remaining -= 4; // 18 bytes used, 171 bytes left

	// Pad the rest of the packet.
	uint8_t pad[1] = {0xFF};
	while (pkt_remaining > 0) {
		PushingStream.write((char*)pad, 1);
		pkt_remaining--;
	}
}

void Pests::WritePMT(int *TrackIds, uint8_t *TrackTypes, int TrackCount) {
	if (!PushingStream || !PushingStream.is_open()) return;

	int pkt_remaining = 188;
	pkt_remaining -= this->WriteTransportHeader(0x0FFF, true, false); // PID of 0x1000 is PMT, as defined int PAT.

	int section_length = (5 * TrackCount) + 9 + 4;

	uint8_t PMT_head[13] = {
		0x00,	// Pointer
		0x02,	// Table ID ( = PMT)
		0xB0 | ((section_length >> 8) & 0x0F), section_length & 0xFF,	// Reserved + section length
		0x00, 0x01, // Program Number (as set in PAT)
		0xC1, // '11', version = 0, current indicator set
		0x00, 0x00, // section number, prev section (both 0)
		0xE, 0x00, // '111', no PCR_PID / was 0xe100
		0xF0, 0x00 // '1111', no program info
	};

	PushingStream.write((char*)PMT_head, 13);
	pkt_remaining -= 13;

	int *tid = TrackIds;
	uint8_t *typ = TrackTypes;
	uint8_t tpid[2] = {0x00, 0x00};
	for (int i = 0; i < TrackCount; i++) {
		PushingStream.write((char*)typ, 1);
		int temp = *tid;
		tpid[0] = 0xE0 | ((temp >> 8) & 0x1F); tpid[1] = temp & 0xFF; // '111', track id
		PushingStream.write((char*)tpid, 2);
		tpid[0] = 0xF0; tpid[1] = 0x00; // '1111', no ES info
		PushingStream.write((char*)tpid, 2);

		typ++;
		tid++;
		pkt_remaining -= 5;
		if (pkt_remaining < 9) break; // run out of space
	}

	// Fake CRC
	uint8_t CRC[4] = {0x00, 0x00, 0x00, 0x00};
	PushingStream.write((char*)CRC, 4);
	pkt_remaining -= 4;

	// Pad the rest of the packet.
	uint8_t pad[1] = {0xFF};
	while (pkt_remaining > 0) {
		PushingStream.write((char*)pad, 1);
		pkt_remaining--;
	}
}

void Pests::PushStream(int TrackId, uint8_t StreamType, uint8_t* data, int Length, long Timestamp){
	if (!PushingStream || !PushingStream.is_open()) return;

	int pkt_remaining = 188;
	int dat_remaining = Length;
	char* position = (char*)data; // pointer to data remaining
	
	pkt_remaining -= this->WriteTransportHeader(TrackId, true, false);
	pkt_remaining -= this->WriteStreamHeader(StreamType, Length, Timestamp);

	int odd = 0;

	while (dat_remaining > 0) {
		if (dat_remaining > pkt_remaining) { // write as much as will fit in the packet
			PushingStream.write(position, pkt_remaining);
			position += pkt_remaining;
			dat_remaining -= pkt_remaining;
			pkt_remaining = 0;
		} else { // write any data we have left
			PushingStream.write(position, dat_remaining);
			position += dat_remaining;
			pkt_remaining -= dat_remaining;
			dat_remaining = 0;
		}

		if (pkt_remaining == 0 && dat_remaining > 0) { // start a new packet.
			pkt_remaining = 188;
			pkt_remaining -= this->WriteTransportHeader(TrackId, false, odd);
			odd = 1 - odd;
		}
		
	}

	// Pad the rest of the packet.
	uint8_t pad[1] = {0xFF};
	while (pkt_remaining > 0) {
		PushingStream.write((char*)pad, 1);
		pkt_remaining--;
	}
	PushingStream.flush();
}

int Pests::WriteStreamHeader(uint8_t StreamType, int DataLength, long Timestamp) {

	uint8_t converted_PTS[5] = { // Timestamp only uses 32 bits, but is stretched to 40...
		0x21 + ((Timestamp >> 29) & 0x0E),
		(Timestamp >> 22) & 0xFF,
		0x01 + ((Timestamp >> 14) & 0xFE),
		(Timestamp >> 7) & 0xFF,
		0x01 + ((Timestamp << 1) & 0xFE)
	};

	int all_in = DataLength + 3 + 5; // plus flags, plus PTS. Only the first 16 bits are used, so packets are limited to ~16kB

	int header_len = 5;

	uint8_t m2ts_type = 0x00;
	switch (StreamType) {
		case Pests::TT_H264:
		case Pests::TT_MpegVideo:
			m2ts_type = 0xE0;
			
			all_in += 6;
			break;
		case Pests::TT_AC3:
		case Pests::TT_MpegAudio:
			m2ts_type = 0xC0;
			break;
	}

	uint8_t basic_header[9] = {
		0x00, 0x00, 0x01, // Start marker
		m2ts_type, // general frame type

		(all_in >> 8) & 0xFF, all_in & 0xFF, // length of all data 

		0x80, // '10', no scramble, normal priority, no data align, no copyright, original.
		0x80, // PTS only (can't use B frames), no clock ref, no data rate, no trick mode, no extra copy info, no CRC, no PES extension.

		(uint8_t)header_len // Length of packet header extra data
	};
	
	if (DataLength > 0xFFFF) { // support large data by specifying 0 here.
		basic_header[4] = 0;
		basic_header[5] = 0;
	}

	PushingStream.write((char*)basic_header, 9);
	PushingStream.write((char*)converted_PTS, 5);

	// If stream is video, insert a dummy sequence header. This is required to work with iPhone.
	switch (StreamType) {
		case Pests::TT_H264:
			uint8_t seq_Header[6] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xE0};
			PushingStream.write((char*)seq_Header, 6);
			header_len += 6; // just to record it.
			break;
	}

	return header_len + 4 + 5; // length written
}

int Pests::WriteTransportHeader(int PID, int Start, int Adapt) {
	uint8_t head[4] = {0x47, 0x00, 0x00, 0x10}; // [0] is sync, [3] has 'payload' set, as we're never adding null packets.

	head[1] = (PID>>8) & 0x1F;
	head[2] = PID & 0xFF;

	if (Start) head[1] |= 0x40; // Payload start indicator
	if (Adapt) head[3] |= 0x20; // Adaption field included

	int ContinuityCounter = 0;
	// nasty hack until I can fix it properly
	if (PID == 120) ContinuityCounter = vCont;
	if (PID == 121) ContinuityCounter = aCont;

	head[3] |= ContinuityCounter;

	ContinuityCounter++; // increment counter
	ContinuityCounter &= 0x0F; // mask to 4 bytes
	
	if (PID == 120) vCont = ContinuityCounter;
	if (PID == 121) aCont = ContinuityCounter;

	PushingStream.write((char*)head, 4);
	if (!Adapt)	return 4; // length written.

	// Write Adaption field
	uint8_t adap[8] = {
		0x07,	// field length
		0x10,	// flags: no discont, no random, low prio, has PCR, no OPCR, no splice, no private, no extension

		0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // this is the mental PCR. 33 bits set of 42 bits used of 48 stored.
	};

	adap[2] = ( ipcr >> 25 ) & 0xff;
	adap[3] = ( ipcr >> 17 ) & 0xff;
	adap[4] = ( ipcr >> 9  ) & 0xff;
	adap[5] = ( ipcr >> 1  ) & 0xff;
	adap[6] = ( ipcr << 7  ) & 0x80;

	ipcr++; // better value later?

	PushingStream.write((char*)adap, 8);
	return 12; // length written (incl. head)
}
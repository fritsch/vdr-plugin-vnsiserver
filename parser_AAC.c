/*
 *      Copyright (C) 2005-2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <assert.h>
#include "config.h"

#include "parser_AAC.h"

static int aac_sample_rates[16] =
{
  96000, 88200, 64000, 48000, 44100, 32000,
  24000, 22050, 16000, 12000, 11025, 8000, 7350
};


cParserAAC::cParserAAC(int pID, cTSStream *stream, sPtsWrap *ptsWrap, bool observePtsWraps)
 : cParser(pID, stream, ptsWrap, observePtsWraps)
{
  m_Configured                = false;
  m_FrameLengthType           = 0;
  m_PTS                       = 0;
  m_DTS                       = 0;
  m_FrameSize                 = 0;
  m_SampleRate                = 0;
  m_Channels                  = 0;
  m_BitRate                   = 0;
  m_PesBufferInitialSize      = 1920*2;
  m_DetectMuxMode             = false;

  char *detectEnv = getenv("VNSI_AAC_MUXMODE");
  if (detectEnv)
  {
    m_DetectMuxMode = true;
    INFOLOG("cParserAAC - detect muxing mode while parsing");
  }
}

cParserAAC::~cParserAAC()
{

}

void cParserAAC::Parse(sStreamPacket *pkt, sStreamPacket *pkt_side_data)
{
  int p = m_PesParserPtr;
  int l;
  while ((l = m_PesBufferPtr - p) > 8)
  {
    if (FindHeaders(m_PesBuffer + p, l) < 0)
      break;
    p++;
  }
  m_PesParserPtr = p;

  if (m_FoundFrame && l >= m_FrameSize)
  {
    bool streamChange = m_Stream->SetAudioInformation(m_Channels, m_SampleRate, m_BitRate, 0, 0);
    pkt->id       = m_pID;
    pkt->data     = &m_PesBuffer[p];
    pkt->size     = m_FrameSize;
    if(!m_SampleRate)
      m_SampleRate = aac_sample_rates[4];
    pkt->duration = 1024 * 90000 / m_SampleRate;
    pkt->dts      = m_DTS;
    pkt->pts      = m_PTS;
    pkt->streamChange = streamChange;

    m_PesNextFramePtr = p + m_FrameSize;
    m_PesParserPtr = 0;
    m_FoundFrame = false;
  }
}

int cParserAAC::FindHeaders(uint8_t *buf, int buf_size)
{
  if (m_FoundFrame)
    return -1;

  uint8_t *buf_ptr = buf;

  if (m_Stream->Type() == stAACLATM)
  {
    if ((buf_ptr[0] == 0x56 && (buf_ptr[1] & 0xE0) == 0xE0))
    {
      // TODO
      if (buf_size < 16)
        return -1;

      cBitstream bs(buf_ptr, 16 * 8);
      bs.skipBits(11);
      m_FrameSize = bs.readBits(13) + 3;
      if (!ParseLATMAudioMuxElement(&bs))
        return 0;

      m_FoundFrame = true;
      m_DTS = m_curPTS;
      m_PTS = m_curPTS;
      if(!m_SampleRate)
	m_SampleRate = aac_sample_rates[4];
      m_curPTS += 90000 * 1024 / m_SampleRate;
      return -1;
    }
    else if (m_DetectMuxMode && buf_ptr[0] == 0xFF && (buf_ptr[1] & 0xF0) == 0xF0)
    {
      m_Stream->SetType(stAACADTS);
      INFOLOG("cParserAAC::FindHeaders - detected ADTS muxing mode");
      return -1;
    }
  }
  else if (m_Stream->Type() == stAACADTS)
  {
    if(buf_ptr[0] == 0xFF && (buf_ptr[1] & 0xF0) == 0xF0)
    {
      // need at least 7 bytes for header
      if (buf_size < 7)
        return -1;

      cBitstream bs(buf_ptr, 9 * 8);
      bs.skipBits(15);

      // check if CRC is present, means header is 9 byte long
      int noCrc = bs.readBits(1);
      if (!noCrc && (buf_size < 9))
        return -1;

      bs.skipBits(2); // profile
      int SampleRateIndex = bs.readBits(4);
      bs.skipBits(1); // private
      m_Channels = bs.readBits(3);
      bs.skipBits(4);

      m_FrameSize = bs.readBits(13);
      m_SampleRate    = aac_sample_rates[SampleRateIndex & 0x0E];

      m_FoundFrame = true;
      m_DTS = m_curPTS;
      m_PTS = m_curPTS;
      m_curPTS += 90000 * 1024 / m_SampleRate;
      return -1;
    }
    else if (m_DetectMuxMode && buf_ptr[0] == 0x56 && (buf_ptr[1] & 0xE0) == 0xE0)
    {
      m_Stream->SetType(stAACLATM);
      INFOLOG("cParserAAC::FindHeaders - detected LATM muxing mode");
      return -1;
    }
  }
  return 0;
}

bool cParserAAC::ParseLATMAudioMuxElement(cBitstream *bs)
{
  if (!bs->readBits1())
    ReadStreamMuxConfig(bs);

  if (!m_Configured)
    return false;

  return true;
}

void cParserAAC::ReadStreamMuxConfig(cBitstream *bs)
{
  int AudioMuxVersion = bs->readBits(1);
  m_AudioMuxVersion_A = 0;
  if (AudioMuxVersion)                       // audioMuxVersion
    m_AudioMuxVersion_A = bs->readBits(1);

  if(m_AudioMuxVersion_A)
    return;

  if (AudioMuxVersion)
    LATMGetValue(bs);                      // taraFullness

  bs->skipBits(1);                         // allStreamSameTimeFraming = 1
  bs->skipBits(6);                         // numSubFrames = 0
  bs->skipBits(4);                         // numPrograms = 0

  // for each program (which there is only on in DVB)
  bs->skipBits(3);                         // numLayer = 0

  // for each layer (which there is only on in DVB)
  if (!AudioMuxVersion)
    ReadAudioSpecificConfig(bs);
  else
    return;

  // these are not needed... perhaps
  m_FrameLengthType = bs->readBits(3);
  switch (m_FrameLengthType)
  {
    case 0:
      bs->readBits(8);
      break;
    case 1:
      bs->readBits(9);
      break;
    case 3:
    case 4:
    case 5:
      bs->readBits(6);                 // celp_table_index
      break;
    case 6:
    case 7:
      bs->readBits(1);                 // hvxc_table_index
      break;
  }

  if (bs->readBits(1))
  {                   // other data?
    if (AudioMuxVersion == 1)
    {
      LATMGetValue(bs);              // other_data_bits
    }
    else
    {
      int esc;
      do
      {
        esc = bs->readBits(1);
        bs->skipBits(8);
      } while (esc);
    }
  }

  if (bs->readBits(1))                   // crc present?
    bs->skipBits(8);                     // config_crc
  m_Configured = true;
}

void cParserAAC::ReadAudioSpecificConfig(cBitstream *bs)
{
  int aot = bs->readBits(5);
  if (aot == 31)
    aot = 32 + bs->readBits(6);

  int SampleRateIndex = bs->readBits(4);

  if (SampleRateIndex == 0xf)
    m_SampleRate = bs->readBits(24);
  else
    m_SampleRate = aac_sample_rates[SampleRateIndex & 0xf];

  m_Channels = bs->readBits(4);

  if (aot == 5) { // AOT_SBR
    if (bs->readBits(4) == 0xf) { // extensionSamplingFrequencyIndex
      bs->skipBits(24);
    }
    aot = bs->readBits(5); // this is the main object type (i.e. non-extended)
    if (aot == 31)
      aot = 32 + bs->readBits(6);
  }

  if(aot != 2)
    return;

  bs->skipBits(1);      //framelen_flag
  if (bs->readBits1())  // depends_on_coder
    bs->skipBits(14);

  if (bs->readBits(1))  // ext_flag
    bs->skipBits(1);    // ext3_flag
}

void cParserAAC::Reset()
{
  cParser::Reset();
  m_Configured = false;
}

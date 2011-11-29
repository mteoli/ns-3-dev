/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2011 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Manuel Requena <manuel.requena@cttc.es>
 */

#include "ns3/simulator.h"
#include "ns3/log.h"

#include "ns3/lte-rlc-header.h"
#include "ns3/lte-rlc-um.h"
#include "ns3/lte-rlc-sdu-status-tag.h"
#include "ns3/lte-rlc-tag.h"

NS_LOG_COMPONENT_DEFINE ("LteRlcUm");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (LteRlcUm);

LteRlcUm::LteRlcUm ()
  : m_txBufferSize (0),
    m_sequenceNumber (0),
    m_vrUr (0),
    m_vrUx (0),
    m_vrUh (0),
    m_windowSize (512),
    m_expectedSeqNumber (0)
{
  NS_LOG_FUNCTION (this);
  m_reassemblingState = WAITING_S0_FULL;

  Simulator::ScheduleNow (&LteRlcUm::Start, this);
}

LteRlcUm::~LteRlcUm ()
{
}

TypeId
LteRlcUm::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LteRlcUm")
    .SetParent<LteRlc> ()
    .AddConstructor<LteRlcUm> ()
    ;
  return tid;
}


/**
 * RLC SAP
 */

void
LteRlcUm::DoTransmitPdcpPdu (Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this);

  /** Store arrival time */
  Time now = Simulator::Now ();
  RlcTag timeTag (now);
  p->AddPacketTag (timeTag);

  /** Store PDCP PDU */

  LteRlcSduStatusTag tag;
  tag.SetStatus (LteRlcSduStatusTag::FULL_SDU);
  p->AddPacketTag (tag);

  NS_LOG_LOGIC ("Tx Buffer: New packet added");
  m_txBuffer.push_back (p);
  m_txBufferSize += p->GetSize ();
  NS_LOG_LOGIC ("NumOfBuffers = " << m_txBuffer.size() );
  NS_LOG_LOGIC ("txBufferSize = " << m_txBufferSize);

  /** Report Buffer Status */

  RlcTag holTimeTag;
  m_txBuffer.front ()->PeekPacketTag (holTimeTag);
  Time holDelay = now - holTimeTag.GetSenderTimestamp ();

  LteMacSapProvider::ReportBufferStatusParameters r;
  r.rnti = m_rnti;
  r.lcid = m_lcid;
  r.txQueueSize = m_txBufferSize + 2 * m_txBuffer.size (); // Data in tx queue + estimated headers size
  r.txQueueHolDelay = holDelay.GetMilliSeconds () ;
  r.retxQueueSize = 0;
  r.retxQueueHolDelay = 0;
  r.statusPduSize = 0;

  NS_LOG_LOGIC ("Send ReportBufferStatus");
  NS_LOG_LOGIC ("Queue size = " << r.txQueueSize);
  NS_LOG_LOGIC ("HOL delay = " << r.txQueueHolDelay);
  m_macSapProvider->ReportBufferStatus (r);
}


/**
 * MAC SAP
 */

void
LteRlcUm::DoNotifyTxOpportunity (uint32_t bytes)
{
  NS_LOG_FUNCTION (this << bytes);

  if (bytes <= 2)
    {
      // TODO ERROR: Header fix part is 2 bytes, we need more bytes for the data
      NS_LOG_LOGIC ("TX opportunity too small = " << bytes);
      return;
    }

  Ptr<Packet> packet = Create<Packet> ();
  LteRlcHeader rlcHeader;

  // Build Data field
  uint32_t nextSegmentSize = bytes - 2;
  uint32_t nextSegmentId = 1;
  uint32_t dataFieldTotalSize = 0;
  uint32_t dataFieldAddedSize = 0;
  std::vector < Ptr<Packet> > dataField;

  // Remove the first packet from the transmission buffer.
  // If only a segment of the packet is taken, then the remaining is given back later
  if ( m_txBuffer.size () == 0 )
    {
      NS_LOG_LOGIC ("No data pending");
      return;
    }

  NS_LOG_LOGIC ("SDUs in TxBuffer  = " << m_txBuffer.size ());
  NS_LOG_LOGIC ("First SDU buffer  = " << *(m_txBuffer.begin()));
  NS_LOG_LOGIC ("First SDU size    = " << (*(m_txBuffer.begin()))->GetSize ());
  NS_LOG_LOGIC ("Next segment size = " << nextSegmentSize);
  NS_LOG_LOGIC ("Remove SDU from TxBuffer");
  Ptr<Packet> firstSegment = (*(m_txBuffer.begin ()))->Copy ();
  m_txBufferSize -= (*(m_txBuffer.begin()))->GetSize ();
  NS_LOG_LOGIC ("txBufferSize      = " << m_txBufferSize );
  m_txBuffer.erase (m_txBuffer.begin ());

  while ( firstSegment && (firstSegment->GetSize () > 0) && (nextSegmentSize > 0) )
    {
      NS_LOG_LOGIC ("WHILE .. firstSegment && firstSegment->GetSize > 0 && nextSegmentSize > 0 ..");
      NS_LOG_LOGIC ("    FirstSegment size = " << firstSegment->GetSize ());
      NS_LOG_LOGIC ("    Next segment size = " << nextSegmentSize);
      if ( firstSegment->GetSize () > nextSegmentSize )
        {
          NS_LOG_LOGIC ("    IF firstSegment > NextSegmentSize");
          // Segment txBuffer.FirstBuffer and
          // Give back the remaining segment to the transmission buffer
          Ptr<Packet> newSegment = firstSegment->CreateFragment (0, nextSegmentSize);

          // Status tag of the new and remaining segments
          // Note: This is the only place where a PDU is segmented and
          // therefore its status can change
          LteRlcSduStatusTag oldTag, newTag;    // TODO CreateFragment copy the tag???
          firstSegment->RemovePacketTag (oldTag);
          newSegment->RemovePacketTag (newTag);
          if (oldTag.GetStatus () == LteRlcSduStatusTag::FULL_SDU)
            {
              newTag.SetStatus (LteRlcSduStatusTag::FIRST_SEGMENT);
              oldTag.SetStatus (LteRlcSduStatusTag::LAST_SEGMENT);
            }
          else if (oldTag.GetStatus () == LteRlcSduStatusTag::LAST_SEGMENT)
            {
              newTag.SetStatus (LteRlcSduStatusTag::MIDDLE_SEGMENT);
              //oldTag.SetStatus (LteRlcSduStatusTag::LAST_SEGMENT);
            }
          firstSegment->AddPacketTag (oldTag);
          newSegment->AddPacketTag (newTag);  // TODO What happens if we add two tags???

          // Give back the remaining segment to the transmission buffer
          firstSegment->RemoveAtStart (nextSegmentSize);
          m_txBuffer.insert (m_txBuffer.begin (), firstSegment);
          m_txBufferSize += (*(m_txBuffer.begin()))->GetSize ();
          firstSegment = 0; // TODO how to put a null ptr to Packet?

          NS_LOG_LOGIC ("    TX buffer: Give back the remaining segment");
          NS_LOG_LOGIC ("    TX buffers = " << m_txBuffer.size ());
          NS_LOG_LOGIC ("    Front buffer size = " << (*(m_txBuffer.begin()))->GetSize ());
          NS_LOG_LOGIC ("    txBufferSize = " << m_txBufferSize );

          // Add Segment to Data field
          dataFieldAddedSize = newSegment->GetSize ();
          dataFieldTotalSize += dataFieldAddedSize;
          dataField.push_back (newSegment);
          newSegment = 0; // TODO how to put a null ptr to Packet?

          // ExtensionBit (Next_Segment - 1) = 0
          rlcHeader.PushExtensionBit (LteRlcHeader::DATA_FIELD_FOLLOWS);

          // no LengthIndicator for the last one

          nextSegmentSize -= dataFieldAddedSize;
          nextSegmentId++;

          // nextSegmentSize MUST be zero

          // (NO more segments) → exit
          // break;
        }
      else if ( (firstSegment->GetSize () == nextSegmentSize) || (m_txBuffer.size () == 0) )
        {
          NS_LOG_LOGIC ("    IF firstSegment == NextSegmentSize || txBuffer.size == 0");
          // Add txBuffer.FirstBuffer to DataField
          dataFieldAddedSize = firstSegment->GetSize ();
          dataFieldTotalSize += dataFieldAddedSize;
          dataField.push_back (firstSegment);
          firstSegment = 0; // TODO how to put a null ptr to Packet?

          // ExtensionBit (Next_Segment - 1) = 0
          rlcHeader.PushExtensionBit (LteRlcHeader::DATA_FIELD_FOLLOWS);

          // no LengthIndicator for the last one

          nextSegmentSize -= dataFieldAddedSize;
          nextSegmentId++;

          NS_LOG_LOGIC ("        SDUs in TxBuffer  = " << m_txBuffer.size ());
          if (m_txBuffer.size () > 0)
            {
              NS_LOG_LOGIC ("        First SDU buffer  = " << *(m_txBuffer.begin()));
              NS_LOG_LOGIC ("        First SDU size    = " << (*(m_txBuffer.begin()))->GetSize ());
            }
          NS_LOG_LOGIC ("        Next segment size = " << nextSegmentSize << " (should be 0)");

          // nextSegmentSize MUST be zero

          // (NO more segments) → exit
          // break;
        }
      else // (firstSegment->GetSize () < m_nextSegmentSize) && (m_txBuffer.size () > 0)
        {
          NS_LOG_LOGIC ("    IF firstSegment < NextSegmentSize && txBuffer.size > 0");
          // Add txBuffer.FirstBuffer to DataField
          dataFieldAddedSize = firstSegment->GetSize ();
          dataFieldTotalSize += dataFieldAddedSize;
          dataField.push_back (firstSegment);

          // ExtensionBit (Next_Segment - 1) = 1
          rlcHeader.PushExtensionBit (LteRlcHeader::E_LI_FIELDS_FOLLOWS);

          // LengthIndicator (Next_Segment)  = txBuffer.FirstBuffer.length()
          rlcHeader.PushLengthIndicator (firstSegment->GetSize ());

          nextSegmentSize -= ((nextSegmentId % 2) ? (2) : (1)) + dataFieldAddedSize; // TODO???
          nextSegmentId++;

          NS_LOG_LOGIC ("        SDUs in TxBuffer  = " << m_txBuffer.size ());
          if (m_txBuffer.size () > 0)
            {
              NS_LOG_LOGIC ("        First SDU buffer  = " << *(m_txBuffer.begin()));
              NS_LOG_LOGIC ("        First SDU size    = " << (*(m_txBuffer.begin()))->GetSize ());
            }
          NS_LOG_LOGIC ("        Next segment size = " << nextSegmentSize);
          NS_LOG_LOGIC ("        Remove SDU from TxBuffer");

          // (more segments)
          firstSegment = (*(m_txBuffer.begin ()))->Copy ();
          m_txBufferSize -= (*(m_txBuffer.begin()))->GetSize ();
          m_txBuffer.erase (m_txBuffer.begin ());
          NS_LOG_LOGIC ("        txBufferSize = " << m_txBufferSize );
        }

    }

  // Build RLC header
  rlcHeader.SetSequenceNumber ( (m_sequenceNumber++) % 1024 ); // TODO See SetIdentification in ipv4 header

  // Build RLC PDU with DataField and Header
  std::vector< Ptr<Packet> >::iterator it;
  it = dataField.begin ();

  uint8_t framingInfo = 0;

  // FIRST SEGMENT
  LteRlcSduStatusTag tag;
  (*it)->RemovePacketTag (tag);
  if ( (tag.GetStatus () == LteRlcSduStatusTag::FULL_SDU) ||
        (tag.GetStatus () == LteRlcSduStatusTag::FIRST_SEGMENT) )
    {
      framingInfo |= LteRlcHeader::FIRST_BYTE;
    }
  else
    {
      framingInfo |= LteRlcHeader::NO_FIRST_BYTE;
    }
  (*it)->AddPacketTag (tag);

  while (it < dataField.end ())
    {
      NS_LOG_LOGIC ("Adding SDU/segment to packet, length = " << (*it)->GetSize ());

      packet->AddAtEnd (*it);
      it++;
    }

  // LAST SEGMENT (Note: There could be only one and be the first one)
  it--;
  (*it)->RemovePacketTag (tag);
  if ( (tag.GetStatus () == LteRlcSduStatusTag::FULL_SDU) ||
        (tag.GetStatus () == LteRlcSduStatusTag::LAST_SEGMENT) )
    {
      framingInfo |= LteRlcHeader::LAST_BYTE;
    }
  else
    {
      framingInfo |= LteRlcHeader::NO_LAST_BYTE;
    }
  (*it)->AddPacketTag (tag);

  rlcHeader.SetFramingInfo (framingInfo);

  NS_LOG_LOGIC ("RLC header: " << rlcHeader);
  packet->AddHeader (rlcHeader);

  // Sender timestamp
  RlcTag rlcTag (Simulator::Now ());
  packet->AddByteTag (rlcTag);
  m_txPdu (m_rnti, m_lcid, packet->GetSize ());

  // Send RLC PDU to MAC layer
  LteMacSapProvider::TransmitPduParameters params;
  params.pdu = packet;
  params.rnti = m_rnti;
  params.lcid = m_lcid;

  m_macSapProvider->TransmitPdu (params);
}

void
LteRlcUm::DoNotifyHarqDeliveryFailure ()
{
  NS_LOG_FUNCTION (this);
}

void
LteRlcUm::DoReceivePdu (Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this);

  // Receiver timestamp
  RlcTag rlcTag;
  Time delay;
  if (p->FindFirstMatchingByteTag (rlcTag))
    {
      delay = Simulator::Now() - rlcTag.GetSenderTimestamp ();
    }
  m_rxPdu(m_rnti, m_lcid, p->GetSize (), delay.GetNanoSeconds ());

  // 5.1.2.2 Receive operations

  // Get RLC header parameters
  LteRlcHeader rlcHeader;
  p->PeekHeader (rlcHeader);
  uint16_t seqNumber = rlcHeader.GetSequenceNumber ();

  // 5.1.2.2.1 General
  // The receiving UM RLC entity shall maintain a reordering window according to state variable VR(UH) as follows:
  // - a SN falls within the reordering window if (VR(UH)  UM_Window_Size) <= SN < VR(UH);
  // - a SN falls outside of the reordering window otherwise.
  // When receiving an UMD PDU from lower layer, the receiving UM RLC entity shall:
  // - either discard the received UMD PDU or place it in the reception buffer (see sub clause 5.1.2.2.2);
  // - if the received UMD PDU was placed in the reception buffer:
  // - update state variables, reassemble and deliver RLC SDUs to upper layer and start/stop t-Reordering as needed (see sub clause 5.1.2.2.3);
  // When t-Reordering expires, the receiving UM RLC entity shall:
  // - update state variables, reassemble and deliver RLC SDUs to upper layer and start t-Reordering as needed (see sub clause 5.1.2.2.4).

  // 5.1.2.2.2 Actions when an UMD PDU is received from lower layer
  // When an UMD PDU with SN = x is received from lower layer, the receiving UM RLC entity shall:
  // - if VR(UR) < x < VR(UH) and the UMD PDU with SN = x has been received before; or
  // - if (VR(UH) - UM_Window_Size) <= x < VR(UR):
  //    - discard the received UMD PDU;
  // - else:
  //    - place the received UMD PDU in the reception buffer.

  NS_LOG_LOGIC ("VR(UR) = " << m_vrUr);
  NS_LOG_LOGIC ("VR(UX) = " << m_vrUx);
  NS_LOG_LOGIC ("VR(UH) = " << m_vrUh);
  NS_LOG_LOGIC ("SN = " << seqNumber);

  if ( ( (m_vrUr < seqNumber) && (seqNumber < m_vrUh) && (m_rxBuffer.count (seqNumber) > 0) ) ||
       ( ((m_vrUh - m_windowSize) <= seqNumber) && (seqNumber < m_vrUr) )
     )
    {
      // TODO discard the received UMD PDU. How a packet is discarded?
      NS_LOG_LOGIC ("UMD PDU discarded");
      return;
    }
  else
    {
      NS_LOG_LOGIC ("Place UMD PDU in the reception buffer");
      m_rxBuffer[seqNumber] = p;
    }


  // 5.1.2.2.3 Actions when an UMD PDU is placed in the reception buffer
  // When an UMD PDU with SN = x is placed in the reception buffer, the receiving UM RLC entity shall:

  // - if x falls outside of the reordering window:
  //    - update VR(UH) to x + 1;
  //    - reassemble RLC SDUs from any UMD PDUs with SN that falls outside of the reordering window, remove
  //      RLC headers when doing so and deliver the reassembled RLC SDUs to upper layer in ascending order of the
  //      RLC SN if not delivered before;
  //    - if VR(UR) falls outside of the reordering window:
  //        - set VR(UR) to (VR(UH) - UM_Window_Size);

  if ( ! IsInsideReorderingWindow (seqNumber))
    {
      NS_LOG_LOGIC ("SN outside the reordering window");

      m_vrUh = seqNumber + 1;
      NS_LOG_LOGIC ("New VR(UH) = " << m_vrUh);

      ReassembleOutsideWindow ();

      if ( ! IsInsideReorderingWindow (m_vrUr) )
        {
          m_vrUr = m_vrUh - m_windowSize;
          NS_LOG_LOGIC ("VR(UR) outside the reordering window");
          NS_LOG_LOGIC ("New VR(UR) = " << m_vrUr);
        }
    }

  // - if the reception buffer contains an UMD PDU with SN = VR(UR):
  //    - update VR(UR) to the SN of the first UMD PDU with SN > current VR(UR) that has not been received;
  //    - reassemble RLC SDUs from any UMD PDUs with SN < updated VR(UR), remove RLC headers when doing
  //      so and deliver the reassembled RLC SDUs to upper layer in ascending order of the RLC SN if not delivered
  //      before;

  if ( m_rxBuffer.count (m_vrUr) > 0 )
    {
      NS_LOG_LOGIC ("Reception buffer contains SN = " << m_vrUr);

      std::map <uint16_t, Ptr<Packet> >::iterator it;
      uint16_t newVrUr;

      it = m_rxBuffer.find (m_vrUr);
      newVrUr = (it->first) + 1;
      while ( m_rxBuffer.count (newVrUr) > 0 )
        {
          newVrUr++;
        }
      m_vrUr = newVrUr;
      NS_LOG_LOGIC ("New VR(UR) = " << m_vrUr);

      ReassembleSnLessThan (m_vrUr);
    }

  // - if t-Reordering is running:
  //    - if VR(UX) <= VR(UR); or
  //    - if VR(UX) falls outside of the reordering window and VR(UX) is not equal to VR(UH)::
  //        - stop and reset t-Reordering;
  if ( m_reorderingTimer.IsRunning () )
    {
      NS_LOG_LOGIC ("Reordering timer is running");

      if ( (m_vrUx <= m_vrUr) ||
           ((! IsInsideReorderingWindow (m_vrUx)) && (m_vrUx != m_vrUh)) )
        {
          NS_LOG_LOGIC ("Stop reordering timer");
          m_reorderingTimer.Cancel ();
        }
    }

  // - if t-Reordering is not running (includes the case when t-Reordering is stopped due to actions above):
  //    - if VR(UH) > VR(UR):
  //        - start t-Reordering;
  //        - set VR(UX) to VR(UH).
  if ( ! m_reorderingTimer.IsRunning () )
    {
      NS_LOG_LOGIC ("Reordering timer is not running");

      if ( m_vrUx > m_vrUr )
        {
          NS_LOG_LOGIC ("VR(UX) > VR(UR). " << m_vrUx << " > " << m_vrUr);
          NS_LOG_LOGIC ("Start reordering timer");
          m_reorderingTimer = Simulator::Schedule (Time ("1.0s"),
                                                   &LteRlcUm::ExpireReorderingTimer ,this);
          m_vrUx = m_vrUh;
          NS_LOG_LOGIC ("New VR(UX) = " << m_vrUx);
        }
    }

}


void
LteRlcUm::Start ()
{
  NS_LOG_FUNCTION (this);

  LteMacSapProvider::ReportBufferStatusParameters p;
  p.rnti = m_rnti;
  p.lcid = m_lcid;
  p.txQueueSize = 0;
  p.txQueueHolDelay = 0;
  p.retxQueueSize = 0;
  p.retxQueueHolDelay = 0;
  p.statusPduSize = 0;

  m_macSapProvider->ReportBufferStatus (p);
}


bool
LteRlcUm::IsInsideReorderingWindow (uint16_t seqNumber)
{
  if ( ((m_vrUh - m_windowSize) <= seqNumber) && (seqNumber < m_vrUh))
    {
      return true;
    }
  else
    {
      return false;
    }
}


void
LteRlcUm::ReassembleAndDeliver (Ptr<Packet> packet)
{
  LteRlcHeader rlcHeader;
  packet->RemoveHeader (rlcHeader);
  uint8_t framingInfo = rlcHeader.GetFramingInfo ();
  uint16_t currSeqNumber = rlcHeader.GetSequenceNumber (); // TODO Needed if losses
  bool expectedSnLost;

  if ( currSeqNumber != m_expectedSeqNumber )
    {
      expectedSnLost = true;
      // TODO LOSSES
      NS_LOG_LOGIC ("There are losses. Expected SN = " << m_expectedSeqNumber << ". Current SN = " << currSeqNumber);
      m_expectedSeqNumber = currSeqNumber + 1;
    }
  else
    {
      expectedSnLost = false;
      NS_LOG_LOGIC ("No losses. Expected SN = " << m_expectedSeqNumber << ". Current SN = " << currSeqNumber);
      m_expectedSeqNumber++;
    }

  // Build list of SDUs
  uint8_t extensionBit;
  uint16_t lengthIndicator;
  do
    {
      extensionBit = rlcHeader.PopExtensionBit ();
      NS_LOG_LOGIC ("E = " << (uint16_t)extensionBit);

      if ( extensionBit == 0 )
        {
          m_sdusBuffer.push_back (packet);
        }
      else // extensionBit == 1
        {
          lengthIndicator = rlcHeader.PopLengthIndicator ();
          NS_LOG_LOGIC ("LI = " << lengthIndicator);

          // Check if there is enough data in the packet
          if ( lengthIndicator >= packet->GetSize () )
            {
              NS_LOG_LOGIC ("INTERNAL ERROR: Not enough data in the packet (" << packet->GetSize () << "). Needed LI=" << lengthIndicator);
              // TODO What to do in this case? Discard packet and continue? Or Assert?
            }

          // Split packet in two fragments
          Ptr<Packet> data_field = packet->CreateFragment (0, lengthIndicator);
          packet->RemoveAtStart (lengthIndicator);

          m_sdusBuffer.push_back (data_field);
        }
    }
  while ( extensionBit == 1 );

  std::list < Ptr<Packet> >::iterator it;

  // Current reassembling state
  if      (m_reassemblingState == WAITING_S0_FULL)  NS_LOG_LOGIC ("Reassembling State = 'WAITING_S0_FULL'");
  else if (m_reassemblingState == WAITING_SI_SF)    NS_LOG_LOGIC ("Reassembling State = 'WAITING_SI_SF'");
  else                                              NS_LOG_LOGIC ("Reassembling State = Unknown state");

  // Received framing Info
  NS_LOG_LOGIC ("Framing Info = " << (uint16_t)framingInfo);

  // Reassemble the list of SDUs (when there is no losses)
  if (!expectedSnLost)
    {
      switch (m_reassemblingState)
        {
          case WAITING_S0_FULL:
                  switch (framingInfo)
                    {
                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                              * Deliver one or multiple PDUs
                              */
                              for ( it = m_sdusBuffer.begin () ; it != m_sdusBuffer.end () ; it++ )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (*it);
                                }
                              m_sdusBuffer.clear ();
                      break;

                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              m_reassemblingState = WAITING_SI_SF;

                              /**
                              * Deliver full PDUs
                              */
                              while ( m_sdusBuffer.size () > 1 )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }

                              /**
                              * Keep S0
                              */
                              m_keepS0 = m_sdusBuffer.front ();
                              m_sdusBuffer.pop_front ();
                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                      default:
                              /**
                              * ERROR: Transition not possible
                              */
                              NS_LOG_LOGIC ("INTERNAL ERROR: Transition not possible. FI = " << framingInfo);
                      break;
                    }
          break;

          case WAITING_SI_SF:
                  switch (framingInfo)
                    {
                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                              * Deliver (Kept)S0 + SN
                              */
                              m_keepS0->AddAtEnd (m_sdusBuffer.front ());
                              m_sdusBuffer.pop_front ();
                              m_rlcSapUser->ReceivePdcpPdu (m_keepS0);

                              /**
                                * Deliver zero, one or multiple PDUs
                                */
                              while ( ! m_sdusBuffer.empty () )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              m_reassemblingState = WAITING_SI_SF;

                              /**
                              * Keep SI
                              */
                              if ( m_sdusBuffer.size () == 1 )
                                {
                                  m_keepS0->AddAtEnd (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }
                              else // m_sdusBuffer.size () > 1
                                {
                                  /**
                                  * Deliver (Kept)S0 + SN
                                  */
                                  m_keepS0->AddAtEnd (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                  m_rlcSapUser->ReceivePdcpPdu (m_keepS0);

                                  /**
                                  * Deliver zero, one or multiple PDUs
                                  */
                                  while ( m_sdusBuffer.size () > 1 )
                                    {
                                      m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                      m_sdusBuffer.pop_front ();
                                    }

                                  /**
                                  * Keep S0
                                  */
                                  m_keepS0 = m_sdusBuffer.front ();
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                      default:
                              /**
                                * ERROR: Transition not possible
                                */
                              NS_LOG_LOGIC ("INTERNAL ERROR: Transition not possible. FI = " << framingInfo);
                      break;
                    }
          break;

          default:
                NS_LOG_LOGIC ("INTERNAL ERROR: Wrong reassembling state = " << m_reassemblingState);
          break;
        }
    }
  else // Reassemble the list of SDUs (when there are losses, i.e. the received SN is not the expected one)
    {
      switch (m_reassemblingState)
        {
          case WAITING_S0_FULL:
                  switch (framingInfo)
                    {
                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                               * Deliver one or multiple PDUs
                               */
                              for ( it = m_sdusBuffer.begin () ; it != m_sdusBuffer.end () ; it++ )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (*it);
                                }
                              m_sdusBuffer.clear ();
                      break;

                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              m_reassemblingState = WAITING_SI_SF;

                              /**
                               * Deliver full PDUs
                               */
                              while ( m_sdusBuffer.size () > 1 )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }

                              /**
                               * Keep S0
                               */
                              m_keepS0 = m_sdusBuffer.front ();
                              m_sdusBuffer.pop_front ();
                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                               * Discard SN
                               */
                              m_sdusBuffer.pop_front ();

                              /**
                               * Deliver zero, one or multiple PDUs
                               */
                              while ( ! m_sdusBuffer.empty () )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              if ( m_sdusBuffer.size () == 1 )
                                {
                                  m_reassemblingState = WAITING_S0_FULL;
                                }
                              else
                                {
                                  m_reassemblingState = WAITING_SI_SF;
                                }

                              /**
                               * Discard SI or SN
                               */
                              m_sdusBuffer.pop_front ();

                              if ( m_sdusBuffer.size () > 0 )
                                {
                                  /**
                                  * Deliver zero, one or multiple PDUs
                                  */
                                  while ( m_sdusBuffer.size () > 1 )
                                    {
                                      m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                      m_sdusBuffer.pop_front ();
                                    }

                                  /**
                                  * Keep S0
                                  */
                                  m_keepS0 = m_sdusBuffer.front ();
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      default:
                              /**
                               * ERROR: Transition not possible
                               */
                              NS_LOG_LOGIC ("INTERNAL ERROR: Transition not possible. FI = " << framingInfo);
                      break;
                    }
          break;

          case WAITING_SI_SF:
                  switch (framingInfo)
                    {
                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                               * Discard S0
                               */
                              m_keepS0 = 0;

                              /**
                               * Deliver one or multiple PDUs
                               */
                              while ( ! m_sdusBuffer.empty () )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      case (LteRlcHeader::FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              m_reassemblingState = WAITING_SI_SF;

                              /**
                               * Discard S0
                               */
                              m_keepS0 = 0;

                              /**
                               * Deliver zero, one or multiple PDUs
                               */
                              while ( m_sdusBuffer.size () > 1 )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }

                              /**
                               * Keep S0
                               */
                              m_keepS0 = m_sdusBuffer.front ();
                              m_sdusBuffer.pop_front ();

                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::LAST_BYTE):
                              m_reassemblingState = WAITING_S0_FULL;

                              /**
                               * Discard S0
                               */
                              m_keepS0 = 0;

                              /**
                               * Discard SI or SN
                               */
                              m_sdusBuffer.pop_front ();

                              /**
                               * Deliver zero, one or multiple PDUs
                               */
                              while ( ! m_sdusBuffer.empty () )
                                {
                                  m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      case (LteRlcHeader::NO_FIRST_BYTE | LteRlcHeader::NO_LAST_BYTE):
                              if ( m_sdusBuffer.size () == 1 )
                                {
                                  m_reassemblingState = WAITING_S0_FULL;
                                }
                              else
                                {
                                  m_reassemblingState = WAITING_SI_SF;
                                }

                              /**
                               * Discard S0
                               */
                              m_keepS0 = 0;

                              /**
                               * Discard SI or SN
                               */
                              m_sdusBuffer.pop_front ();

                              if ( m_sdusBuffer.size () > 0 )
                                {
                                  /**
                                   * Deliver zero, one or multiple PDUs
                                   */
                                  while ( m_sdusBuffer.size () > 1 )
                                    {
                                      m_rlcSapUser->ReceivePdcpPdu (m_sdusBuffer.front ());
                                      m_sdusBuffer.pop_front ();
                                    }

                                  /**
                                   * Keep S0
                                   */
                                  m_keepS0 = m_sdusBuffer.front ();
                                  m_sdusBuffer.pop_front ();
                                }
                      break;

                      default:
                              /**
                                * ERROR: Transition not possible
                                */
                              NS_LOG_LOGIC ("INTERNAL ERROR: Transition not possible. FI = " << framingInfo);
                      break;
                    }
          break;

          default:
                NS_LOG_LOGIC ("INTERNAL ERROR: Wrong reassembling state = " << m_reassemblingState);
          break;
        }
    }

}


void
LteRlcUm::ReassembleOutsideWindow (void)
{
  NS_LOG_LOGIC ("Reassemble Outside Window");

  std::map <uint16_t, Ptr<Packet> >::iterator it;
  it = m_rxBuffer.begin ();

  while ( (it != m_rxBuffer.end ()) && ! IsInsideReorderingWindow (it->first) )
    {
      NS_LOG_LOGIC ("SN = " << it->first);

      // Reassemble RLC SDUs and deliver the PDCP PDU to upper layer
      ReassembleAndDeliver (it->second);
      m_rxBuffer.erase (it);
      it++;
    }

  if (it != m_rxBuffer.end ())
    {
      NS_LOG_LOGIC ("(SN = " << it->first << ") is inside the reordering window");
    }
}

void
LteRlcUm::ReassembleSnLessThan (uint16_t seqNumber)
{
  NS_LOG_LOGIC ("Reassemble SN < updated VR(UR)" );

  std::map <uint16_t, Ptr<Packet> >::iterator it;
  it = m_rxBuffer.begin ();

  while ( (it != m_rxBuffer.end ()) && (it->first < seqNumber) )
    {
      NS_LOG_LOGIC ("SN = " << it->first);

      // Reassemble RLC SDUs and deliver the PDCP PDU to upper layer
      ReassembleAndDeliver (it->second);
      m_rxBuffer.erase (it);
      it++;
    }

  if (it != m_rxBuffer.end ())
    {
      NS_LOG_LOGIC ("(SN = " << it->first << ") >= " << m_vrUr);
    }
}


void
LteRlcUm::ExpireReorderingTimer (void)
{
  NS_LOG_LOGIC ("TODO: Reordering Timer has expired...");

  // 5.1.2.2.4 Actions when t-Reordering expires
  // When t-Reordering expires, the receiving UM RLC entity shall:
  // - update VR(UR) to the SN of the first UMD PDU with SN >= VR(UX) that has not been received;
  // - reassemble RLC SDUs from any UMD PDUs with SN < updated VR(UR), remove RLC headers when doing so
  //   and deliver the reassembled RLC SDUs to upper layer in ascending order of the RLC SN if not delivered before;
  // - if VR(UH) > VR(UR):
  //    - start t-Reordering;
  //    - set VR(UX) to VR(UH).

  std::map <uint16_t, Ptr<Packet> >::iterator it;
  uint16_t newVrUr = m_vrUx;

  while ( (it = m_rxBuffer.find (newVrUr)) != m_rxBuffer.end () )
    {
      newVrUr++;
    }
  m_vrUr = newVrUr;

  ReassembleSnLessThan (m_vrUr);

  if ( m_vrUh > m_vrUr)
    {
      NS_LOG_LOGIC ("Start reordering timer");
      m_reorderingTimer = Simulator::Schedule (Time ("1.0s"),
                                               &LteRlcUm::ExpireReorderingTimer, this);
      m_vrUx = m_vrUh;
    }
}


} // namespace ns3

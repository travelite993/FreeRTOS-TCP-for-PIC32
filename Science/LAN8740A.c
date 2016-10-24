/*
 * LAN8740A PHY Driver
 *
 * Copyright (c) 2016 John Robertson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// RTOS
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
// TCP/IP Stack
#include <FreeRTOS_IP.h>
#include <FreeRTOS_IP_Private.h>
// C Runtime
#include <stdint.h>
#include <stdbool.h>

#include "Ethernet.h"
#include "LAN8740A.h"

// Software reset corrupts these and so they must be manually
// set each time or TDR won't function correctly
#define TDR_MATCH_THRESHOLD_VALUE       0x0249U
#define TDR_SHORT_OPEN_THRESHOLD_VALUE  0x0132U

// Propagation constants indexed by tdr_cable_type_t
static const float pCABLE_OPEN_PROP_CONST[_PHY_TDR_CABLE_TYPES] = {
    0.769f, 0.85f, 0.76f, 0.745f
};

static const float pCABLE_SHORT_PROP_CONST[_PHY_TDR_CABLE_TYPES] = {
    0.793f, 0.873f, 0.788f, 0.759f
};

static const uint16_t pCABLE_CBLN_LOOKUP[] = {
    0U, 0U, 0U, 0U, 6U, 17U, 27U, 38U, 49U, 59U, 70U, 81U, 91U, 102U, 113U, 123U
};

const uint16_t nPHY_ADDRESS = 0;

void __attribute__(( interrupt(IPL0AUTO), vector(_EXTERNAL_4_VECTOR) )) PHYInterruptWrapper(void);

void PHYInitialise(void)
{
    LAN8740_CLEAR_HW_RESET();

    PHYWrite(PHY_REG_BASIC_CONTROL, PHY_CTRL_RESET);

    while( PHYRead(PHY_REG_BASIC_CONTROL) & PHY_CTRL_RESET );

    PHY_MMDWrite(LAN8740_MMD_DEVAD_VENDOR, LAN8740_MMDINDX_VENDOR_TDR_MATCH_THRESHOLD, TDR_MATCH_THRESHOLD_VALUE);
    PHY_MMDWrite(LAN8740_MMD_DEVAD_VENDOR, LAN8740_MMDINDX_VENDOR_TDR_SHORT_OPEN_THRESHOLD, TDR_SHORT_OPEN_THRESHOLD_VALUE);

    uint16_t wucsr = PHY_MMDRead(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_WAKEUP_CTRL_STATUS);
    PHY_MMDWrite(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_WAKEUP_CTRL_STATUS, wucsr & ~LAN8740_MMD_PCS_WUCSR_MAGIC_PACKET_EN);

    PHY_MMDWrite(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_MAC_RX_ADDR_A, EMAC1SA0);
    PHY_MMDWrite(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_MAC_RX_ADDR_B, EMAC1SA1);
    PHY_MMDWrite(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_MAC_RX_ADDR_C, EMAC1SA2);

    IPC5bits.INT4IP = configKERNEL_INTERRUPT_PRIORITY;

    IFS0CLR = _IFS0_INT4IF_MASK;
    IEC0SET = _IEC0_INT4IE_MASK;

    PHYWrite(LAN8740_REG_INTERRUPT_MASK, LAN8740_INT_AUTO_NEG_COMPLETE | LAN8740_INT_LINK_DOWN);
}

void PHYGetStatus(phy_status_t *pStatus)
{
    uint16_t linkStat = PHYRead(LAN8740_REG_PHY_SPECIAL_CONTROL_STATUS);

    pStatus->speed = linkStat & LAN8740_SCS_SPEED_100M ? PHY_SPEED_100MBPS : PHY_SPEED_10MBPS;
    pStatus->fullDuplex = (linkStat & LAN8740_SCS_FULL_DUPLEX) != 0;
}

static void AssessCable(phy_tdr_cable_t type, phy_tdr_state_t *pState, float *pLenEst)
{
    // Read register first to work around an issue where the TDR never starts/completes
    PHYRead(LAN8740_REG_TDR_CONTROL_STATUS);

    PHYWrite(LAN8740_REG_TDR_CONTROL_STATUS, LAN8740_TCS_TDR_ENABLE);

    uint16_t tdr;

    do
    {
        vTaskDelay( pdMS_TO_TICKS(100) );
        tdr = PHYRead(LAN8740_REG_TDR_CONTROL_STATUS);
    }
    while((tdr & LAN8740_TCS_TDR_OPERATION_COMPLETE) == 0);

    switch(tdr & LAN8740_TCS_TDR_CHANNEL_CABLE_MASK)
    {
    case LAN8740_TCS_TDR_CHANNEL_CABLE_SHORTED:
        *pState = PHY_TDR_STATE_SHORTED;
        *pLenEst = pCABLE_SHORT_PROP_CONST[type] * (tdr & LAN8740_TCS_TDR_CHANNEL_LENGTH_MASK);
        break;

    case LAN8740_TCS_TDR_CHANNEL_CABLE_OPEN:
        *pState = PHY_TDR_STATE_OPEN;
        *pLenEst = pCABLE_OPEN_PROP_CONST[type] * (tdr & LAN8740_TCS_TDR_CHANNEL_LENGTH_MASK);
        break;

    case LAN8740_TCS_TDR_CHANNEL_CABLE_MATCHED:
        *pState = PHY_TDR_STATE_GOOD;
        *pLenEst = -1.0f;
        break;

    default:
        *pState = PHY_TDR_STATE_FAILED;
        *pLenEst = -1.0f;
    }
}

phy_tdr_state_t PHYCableDiagnostic(phy_tdr_cable_t type, float *pLenEstimate)
{
    if((type < 0) || (type >= _PHY_TDR_CABLE_TYPES))
    {
        return PHY_TDR_STATE_ERROR;
    }

    // If link is up, don't need to do any TDR
    uint16_t status = PHYRead(PHY_REG_BASIC_STATUS);
    uint16_t linkUpCount = 20;

    while( linkUpCount && (status & PHY_STAT_LINK_IS_UP) )
    {
        vTaskDelay( pdMS_TO_TICKS(50) );

        linkUpCount--;
        status = PHYRead(PHY_REG_BASIC_STATUS);
    }

    if(linkUpCount == 0)
    {
        if( pLenEstimate )
        {
            uint16_t linkStat = PHYRead(LAN8740_REG_PHY_SPECIAL_CONTROL_STATUS);

            if((linkStat & LAN8740_SCS_HCDSPEED_MASK) == (LAN8740_SCS_SPEED_100M | LAN8740_SCS_FULL_DUPLEX))
            {
                uint16_t cbln = PHYRead(LAN8740_REG_CABLE_LENGTH);
                *pLenEstimate = pCABLE_CBLN_LOOKUP[(cbln & LAN8740_CL_LENGTH_MASK) >> LAN8740_CL_LENGTH_POSN];
            }
            else
            {
                *pLenEstimate = -1.0;
            }
        }

        return PHY_TDR_STATE_GOOD;
    }

    // No link, so try TDR on each signal pair
    uint16_t origBasicControl = PHYRead(PHY_REG_BASIC_CONTROL);
    uint16_t origSpecialCtrlStat = PHYRead(LAN8740_REG_SPECIAL_CONTROL_STATUS_INDICATIONS);

    PHYWrite(PHY_REG_BASIC_CONTROL, PHY_CTRL_SPEED_100MBPS | PHY_CTRL_FULL_DUPLEX);
    PHYWrite(LAN8740_REG_SPECIAL_CONTROL_STATUS_INDICATIONS, LAN8740_SCSI_DISABLE_AUTO_MDIX);

    phy_tdr_state_t state; float fLenEst;
    AssessCable(type, &state, &fLenEst);

    PHYWrite(LAN8740_REG_SPECIAL_CONTROL_STATUS_INDICATIONS, LAN8740_SCSI_DISABLE_AUTO_MDIX | LAN8740_SCSI_MDIX);

    phy_tdr_state_t state_mdix; float fLenEst_mdix;
    AssessCable(type, &state_mdix, &fLenEst_mdix);

    PHYWrite(LAN8740_REG_SPECIAL_CONTROL_STATUS_INDICATIONS, origSpecialCtrlStat);
    PHYWrite(PHY_REG_BASIC_CONTROL, origBasicControl);

    bool bUseMDIXResult = false;

    if(state == state_mdix)
    {
        if(fLenEst_mdix > fLenEst)
        {
            bUseMDIXResult = true;
        }
    }
    else
    {
        if(state == PHY_TDR_STATE_GOOD)
        {
            bUseMDIXResult = true;
        }
        else if(state_mdix != PHY_TDR_STATE_GOOD)
        {
            if(fLenEst_mdix > fLenEst)
                bUseMDIXResult = true;
        }
    }

    if( bUseMDIXResult )
    {
        fLenEst = fLenEst_mdix;
        state = state_mdix;
    }

    if( pLenEstimate )
        *pLenEstimate = fLenEst;

    return state;
}

void PHYInterruptHandler(void)
{
    IFS0CLR = _IFS0_INT4IF_MASK;

    uint16_t intSource = PHYRead(LAN8740_REG_INTERRUPT_SOURCE_FLAG);

    BaseType_t bHigherPriorityTaskWoken = pdFALSE;

    if(g_interfaceState == ETH_NORMAL)
    {
        if(intSource & (LAN8740_INT_AUTO_NEG_COMPLETE | LAN8740_INT_LINK_DOWN))
        {
            uint16_t bs = PHYRead(PHY_REG_BASIC_STATUS);

            if(bs & PHY_STAT_LINK_IS_UP)
            {
                xSemaphoreGiveFromISR(g_hLinkUpSemaphore, &bHigherPriorityTaskWoken);
            }
            else
            {
                if( FreeRTOS_NetworkDownFromISR() )
                    bHigherPriorityTaskWoken = pdTRUE;
            }
        }
    }
    else if(g_interfaceState == ETH_WAKE_ON_LAN)
    {
        if(intSource & LAN8740_INT_WOL)
        {
            g_interfaceState = ETH_WAKE_ON_LAN_WOKEN;
            xSemaphoreGiveFromISR(g_hLinkUpSemaphore, &bHigherPriorityTaskWoken);
        }
    }

    portEND_SWITCHING_ISR(bHigherPriorityTaskWoken);
}

bool PHYSupportsWOL(void)
{
    return true;
}

void PHYPrepareWakeOnLAN(void)
{
    IEC0CLR = _IEC0_INT4IE_MASK;

    uint16_t wucsr = PHY_MMDRead(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_WAKEUP_CTRL_STATUS);
    PHY_MMDWrite(LAN8740_MMD_DEVAD_PCS, LAN8740_MMDINDX_PCS_WAKEUP_CTRL_STATUS, wucsr | LAN8740_MMD_PCS_WUCSR_MAGIC_PACKET_EN);

    PHYWrite(LAN8740_REG_INTERRUPT_MASK, LAN8740_INT_WOL);
    PHYRead(LAN8740_REG_INTERRUPT_SOURCE_FLAG);

    IFS0CLR = _IFS0_INT4IF_MASK;
    IEC0SET = _IEC0_INT4IE_MASK;
}

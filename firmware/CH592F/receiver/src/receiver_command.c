#include "kbd_command.h"
#include "kbd_types.h"
#include "kbd_mode_config.h"
#include "usb_hid.h"
#include "receiver_radio.h"
#include "receiver_log.h"
#include <string.h>

static bool s_deferred_response;
static uint8_t s_deferred_cmd;
static uint8_t s_deferred_sub;

void Receiver_Command_ProcessDeferred(void)
{
    int result;
    if (!s_deferred_response || !Receiver_Radio_TakeControlResult(&result)) return;
    uint8_t resp = result == 0 ? KBD_RESP_OK : KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(s_deferred_cmd, s_deferred_sub, &resp, 1);
    s_deferred_response = false;
}

void KBD_Command_Init(void) {}
void KBD_Command_SetResponseSender(kbd_command_response_sender_t sender) { (void)sender; }
void KBD_Command_SendResponse(uint8_t cmd, uint8_t sub, const uint8_t *data, uint8_t len)
{
    uint8_t packet[63] = {0}; packet[0] = sub; packet[1] = len;
    if (data && len) memcpy(&packet[2], data, len > 61 ? 61 : len);
    USB_Config_SendResponse(cmd, packet, (uint8_t)(2 + (len > 61 ? 61 : len)));
}
int KBD_Command_Process(const kbd_cmd_frame_t *frame)
{
    uint8_t resp[18] = { KBD_RESP_OK };
    int ret = 0; uint8_t len = 1;
    Receiver_Log_MarkHostSeen();
    if (s_deferred_response &&
        (frame->cmd == KBD_CMD_RADIO_PAIR_START ||
         frame->cmd == KBD_CMD_RADIO_PAIR_CANCEL ||
         frame->cmd == KBD_CMD_RADIO_PAIR_CLEAR ||
         frame->cmd == KBD_CMD_RADIO_POLL_RATE_SET)) {
        resp[0] = KBD_RESP_ERR_BUSY;
        KBD_Command_SendResponse(frame->cmd, frame->sub, resp, 1);
        return -1;
    }
    switch (frame->cmd) {
    case KBD_CMD_SYS_INFO:
        resp[1]=(uint8_t)(KBD_VENDOR_ID>>8); resp[2]=(uint8_t)KBD_VENDOR_ID;
        resp[3]=0x21; resp[4]=0x08;
        resp[5]=KBD_VERSION_MAJOR; resp[6]=KBD_VERSION_MINOR; resp[7]=KBD_VERSION_PATCH;
        resp[8]=0; resp[9]=0; resp[10]=0; resp[11]=0; resp[12]=0; resp[13]=0;
        resp[14]=(KBD_RECEIVER_STARTUP_STAGE >= 3) ? 1 : 0;
        resp[15]=1; len=18; break;
    case KBD_CMD_SYS_STATUS:
        resp[1]=KBD_WORK_MODE_2G4;
        resp[2]=(Receiver_Radio_GetState()==KBD_RADIO_PAIR_CONNECTED) ? KBD_CONN_CONNECTED : KBD_CONN_DISCONNECTED;
        /* Report the last initialization checkpoint actually reached. */
        resp[6]=Receiver_Log_GetCompletedStage();
        resp[7]=Receiver_Radio_GetHostStartupState();
        resp[8]=Receiver_Radio_GetHostStartupResult();
        len=9; break;
    case KBD_CMD_LOG_GET:
        /* Receiver-only: read one retained boot diagnostic.
         * [OK][has entry][event][stage][result].  Empty is still OK. */
        resp[1] = Receiver_Log_Pop(&resp[2], &resp[3], &resp[4]);
        len = resp[1] ? 5 : 2;
        break;
    case KBD_CMD_RADIO_CAPS:
        resp[1]=(KBD_RECEIVER_STARTUP_STAGE >= 3) ? 1 : 0;
        resp[2]=1; resp[3]=4;
        resp[4]=125; resp[5]=0; resp[6]=250; resp[7]=0;
        resp[8]=244; resp[9]=1; resp[10]=232; resp[11]=3; len=12; break;
    case KBD_CMD_RADIO_PAIR_STATUS:
        resp[1]=(uint8_t)Receiver_Radio_GetState(); len=8; break;
    case KBD_CMD_RADIO_PAIR_START:
    case KBD_CMD_RADIO_PAIR_CANCEL:
    case KBD_CMD_RADIO_PAIR_CLEAR:
#if KBD_RECEIVER_STARTUP_STAGE >= 3
        if (frame->cmd == KBD_CMD_RADIO_PAIR_START) ret=Receiver_Radio_StartPairing();
        else if (frame->cmd == KBD_CMD_RADIO_PAIR_CANCEL) ret=Receiver_Radio_CancelPairing();
        else ret=Receiver_Radio_ClearPairing();
#if defined(KBD_RECEIVER_MANUAL_HOST_DIAGNOSTIC)
        /* The diagnostic build acknowledges queueing before RFBound_StartHost
         * runs.  This prevents a blocking vendor call from being reported as
         * a WebHID timeout; SYS_STATUS carries the actual result. */
        if (ret == 0) {
            KBD_Command_SendResponse(frame->cmd, frame->sub, resp, 1);
            return 0;
        }
#endif
        goto deferred;
#else
        ret=-1; break;
#endif
    case KBD_CMD_RADIO_POLL_RATE_GET: {
        uint16_t rate=Receiver_Radio_GetPollRate(); resp[1]=(uint8_t)rate; resp[2]=(uint8_t)(rate>>8); len=3; break;
    }
    case KBD_CMD_RADIO_POLL_RATE_SET:
#if KBD_RECEIVER_STARTUP_STAGE >= 3
        if (frame->len < 2) ret=-1;
        else ret=Receiver_Radio_SetPollRate((uint16_t)frame->data[0] | ((uint16_t)frame->data[1]<<8));
        goto deferred;
#else
        ret=-1; break;
#endif
    default: ret=-1; break;
    }
    if (ret) resp[0]=KBD_RESP_ERR_INVALID;
    KBD_Command_SendResponse(frame->cmd, frame->sub, resp, len);
    return ret;

#if KBD_RECEIVER_STARTUP_STAGE >= 3
deferred:
    if (ret != 0 || s_deferred_response) {
        resp[0] = KBD_RESP_ERR_BUSY;
        KBD_Command_SendResponse(frame->cmd, frame->sub, resp, 1);
        return -1;
    }
    s_deferred_cmd = frame->cmd;
    s_deferred_sub = frame->sub;
    s_deferred_response = true;
    return 0;
#endif
}

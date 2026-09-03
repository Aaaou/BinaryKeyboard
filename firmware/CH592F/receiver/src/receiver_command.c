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
static uint8_t s_mgmt_transaction;
static uint8_t s_mgmt_command;
static uint8_t s_mgmt_sub;
static void receiver_mgmt_response(uint8_t transaction, const uint8_t *frame, uint8_t len, bool error)
{
    if (transaction != s_mgmt_transaction) return;
    if (error || !frame || len < 3u) {
        uint8_t status = error ? KBD_RESP_ERR_INVALID : KBD_RESP_ERR_BUSY;
        KBD_Command_SendResponse(s_mgmt_command, s_mgmt_sub, &status, 1u);
        Receiver_Radio_ResetManagement();
        return;
    }
    if (frame[0] != s_mgmt_command || frame[1] != s_mgmt_sub || frame[2] > 61u || (uint16_t)frame[2] + 3u > len) {
        uint8_t status = KBD_RESP_ERR_INVALID;
        KBD_Command_SendResponse(s_mgmt_command, s_mgmt_sub, &status, 1u);
        Receiver_Radio_ResetManagement();
        return;
    }
    USB_Config_SendResponse(frame[0], &frame[1], (uint8_t)((len - 1u) > 63u ? 63u : len - 1u));
    Receiver_Radio_ResetManagement();
}

/* Reserved command range. The production receiver is ISP-only and must
 * return a deterministic error to legacy Studio builds that still probe IAP. */
enum {
    RECEIVER_CMD_IAP_INFO = 0x80,
    RECEIVER_CMD_IAP_PREPARE = 0x81,
    RECEIVER_CMD_IAP_WRITE = 0x82,
    RECEIVER_CMD_IAP_VERIFY = 0x83,
    RECEIVER_CMD_IAP_ACTIVATE = 0x84,
};

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
    uint8_t resp[61] = { KBD_RESP_OK };
    int ret = 0; uint8_t len = 1;
    Receiver_Log_MarkHostSeen();
    /* Only keyboard configuration commands cross the RF tunnel.  Keep
     * receiver diagnostics (LOG_*), pairing and poll-rate controls local;
     * treating the numeric range as one contiguous block made LOG_GET turn
     * into a remote request and produced BUSY/timeout errors on an unpaired
     * receiver. */
    bool remote_command =
        frame->cmd == KBD_CMD_RADIO_REMOTE_CAPS ||
        (frame->cmd >= KBD_CMD_CFG_SAVE && frame->cmd <= KBD_CMD_CFG_OS_SET) ||
        (frame->cmd >= KBD_CMD_KEYMAP_GET && frame->cmd <= KBD_CMD_LAYER_SET) ||
        (frame->cmd >= KBD_CMD_RGB_GET && frame->cmd <= KBD_CMD_RGB_SET) ||
        (frame->cmd >= KBD_CMD_MACRO_INFO && frame->cmd <= KBD_CMD_MACRO_DEL) ||
        (frame->cmd >= KBD_CMD_FNKEY_GET && frame->cmd <= KBD_CMD_FNKEY_SET) ||
        frame->cmd == KBD_CMD_BATTERY ||
        (frame->cmd >= KBD_CMD_DATAFLASH_INFO && frame->cmd <= KBD_CMD_DATAFLASH_WRITE);
    if (remote_command) {
        Receiver_Log_Event(RX_LOG_MGMT_REQUEST, KBD_RECEIVER_STARTUP_STAGE,
                           frame->cmd);
        if (frame->cmd == KBD_CMD_RADIO_REMOTE_CAPS && Receiver_Radio_HasRemoteCapabilities()) {
            uint8_t caps[8], remote[18] = { KBD_RESP_OK };
            Receiver_Radio_GetRemoteCapabilities(caps);
            remote[1] = (uint8_t)(KBD_VENDOR_ID >> 8); remote[2] = (uint8_t)KBD_VENDOR_ID;
            remote[3] = 0x21; remote[4] = 0x07;
            remote[5] = caps[1]; remote[6] = caps[2]; remote[7] = caps[3];
            remote[8] = caps[6]; remote[9] = KBD_MAX_KEYS; remote[10] = 0;
            remote[11] = caps[4]; remote[12] = caps[5];
            remote[13] = (caps[7] & 0x01u) ? 2u : 0u;
            remote[14] = (caps[7] & 0x02u) ? 1u : 0u; remote[15] = 0u;
            KBD_Command_SendResponse(KBD_CMD_RADIO_REMOTE_CAPS, frame->sub, remote, sizeof(remote));
            return 0;
        }
        /* RFBound may report BOUND between application packets even though
         * the host role is alive and the paired keyboard can receive the
         * next management frame.  Do not gate the tunnel on the transient
         * CONNECTED state; an empty binding is the only state that must be
         * rejected immediately.  A real link failure is reported by the
         * management timeout path. */
        /* Local pairing/poll-rate operations use a separate deferred USB
         * completion path. They must not lock the RF management tunnel: a
         * completed local command can otherwise leave s_deferred_response
         * set until a control callback arrives and make every remote command
         * return ERR_BUSY indefinitely. Receiver_Radio_ManagementBusy() is
         * the sole lock for the remote transaction itself. */
        if (!Receiver_Radio_HasPeer()) {
            Receiver_Log_Event(RX_LOG_MGMT_REJECT, KBD_RECEIVER_STARTUP_STAGE,
                               frame->cmd);
            uint8_t busy = KBD_RESP_ERR_BUSY;
            KBD_Command_SendResponse(frame->cmd, frame->sub, &busy, 1);
            return -1;
        }
        s_mgmt_transaction++;
        if (!s_mgmt_transaction) s_mgmt_transaction = 1;
        s_mgmt_command = frame->cmd;
        s_mgmt_sub = frame->sub;
        Receiver_Radio_SetManagementResponseCallback(receiver_mgmt_response);
        if (Receiver_Radio_SendManagement(frame, s_mgmt_transaction) != 0) {
            Receiver_Radio_ResetManagement();
            uint8_t busy = KBD_RESP_ERR_BUSY;
            KBD_Command_SendResponse(frame->cmd, frame->sub, &busy, 1);
            return -1;
        }
        return 0;
    }
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
    case RECEIVER_CMD_IAP_INFO:
    case RECEIVER_CMD_IAP_PREPARE:
    case RECEIVER_CMD_IAP_WRITE:
    case RECEIVER_CMD_IAP_VERIFY:
    case RECEIVER_CMD_IAP_ACTIVATE:
        resp[0] = KBD_RESP_ERR_INVALID;
        KBD_Command_SendResponse(frame->cmd, frame->sub, resp, 1);
        return -1;
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
        /* [OK][state][role][device id][peer id][local 6][peer 6]
         * [fingerprint 4][generation 4][last valid age 4]
         * [link timeout age 4][release queued age 4][release sent age 4]
         * [release busy count 2]. The first 31 bytes remain compatible with
         * protocol v1 clients. */
        uint32_t v;
        resp[1]=(uint8_t)Receiver_Radio_GetState();
        resp[2]=1;
        resp[3]=Receiver_Radio_GetDeviceId();
        resp[4]=Receiver_Radio_GetPeerDeviceId();
        Receiver_Radio_GetLocalId(&resp[5]);
        Receiver_Radio_GetPeerId(&resp[11]);
        {
            v = Receiver_Radio_GetPairFingerprint();
            resp[17]=(uint8_t)v; resp[18]=(uint8_t)(v>>8);
            resp[19]=(uint8_t)(v>>16); resp[20]=(uint8_t)(v>>24);
            v = Receiver_Radio_GetPairGeneration();
            resp[21]=(uint8_t)v; resp[22]=(uint8_t)(v>>8);
            resp[23]=(uint8_t)(v>>16); resp[24]=(uint8_t)(v>>24);
            v = Receiver_Radio_GetLastValidAge();
            resp[25]=(uint8_t)v; resp[26]=(uint8_t)(v>>8);
            resp[27]=(uint8_t)(v>>16); resp[28]=(uint8_t)(v>>24);
        }
        resp[29]=2;
        resp[30]=(Receiver_Radio_HasPeer() ? 0x01 : 0x00) |
                 (Receiver_Radio_GetState()==KBD_RADIO_PAIR_CONNECTED ? 0x02 : 0x00) |
                 (Receiver_Radio_GetState()==KBD_RADIO_PAIRING ? 0x04 : 0x00);
        v = Receiver_Radio_GetLastLinkTimeoutAge();
        resp[31]=(uint8_t)v; resp[32]=(uint8_t)(v>>8);
        resp[33]=(uint8_t)(v>>16); resp[34]=(uint8_t)(v>>24);
        v = Receiver_Radio_GetLastReleaseQueuedAge();
        resp[35]=(uint8_t)v; resp[36]=(uint8_t)(v>>8);
        resp[37]=(uint8_t)(v>>16); resp[38]=(uint8_t)(v>>24);
        v = Receiver_Radio_GetLastReleaseSentAge();
        resp[39]=(uint8_t)v; resp[40]=(uint8_t)(v>>8);
        resp[41]=(uint8_t)(v>>16); resp[42]=(uint8_t)(v>>24);
        v = Receiver_Radio_GetReleaseBusyCount();
        resp[43]=(uint8_t)v; resp[44]=(uint8_t)(v>>8);
        {
            uint8_t md[7];
            Receiver_Radio_GetManagementDiagnostics(md);
            memcpy(&resp[45], md, sizeof(md));
        }
        Receiver_Radio_GetHidDiagnostics(&resp[52]);
        len=61; break;
    case KBD_CMD_RADIO_PAIR_START:
    case KBD_CMD_RADIO_PAIR_CANCEL:
    case KBD_CMD_RADIO_PAIR_CLEAR:
#if KBD_RECEIVER_STARTUP_STAGE >= 3
        if (frame->cmd == KBD_CMD_RADIO_PAIR_START) ret=Receiver_Radio_StartPairing();
        else if (frame->cmd == KBD_CMD_RADIO_PAIR_CANCEL) ret=Receiver_Radio_CancelPairing();
        else ret=Receiver_Radio_ClearPairing();
#if defined(KBD_RECEIVER_EARLY_COMMAND_ACK)
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

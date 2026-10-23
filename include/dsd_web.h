/*-------------------------------------------------------------------------------
 * dsd_web.h
 * DSD-FME Web Interface (HTTP/WebSocket server, spectrum, scanner, auto tune)
 *
 * The web interface runs an embedded HTTP/WebSocket server thread. All decoder
 * state is only touched on the main (decoder) thread inside web_poll, the
 * server thread only moves bytes between sockets and queues.
 *-----------------------------------------------------------------------------*/

#ifndef DSD_WEB_H
#define DSD_WEB_H

#include "dsd.h"

#ifdef __cplusplus
extern "C" {
#endif

//per client queue policy kinds
#define WEB_KIND_CTRL    0
#define WEB_KIND_STATUS  1
#define WEB_KIND_FFT     2
#define WEB_KIND_AUDIO   3
#define WEB_KIND_CONSOLE 4

//client subscription bits
#define WEB_SUB_FFT      0x01
#define WEB_SUB_AUDIO    0x02
#define WEB_SUB_CONSOLE  0x04
#define WEB_SUB_EVENTS   0x08 //the decoder's own event history in the status snapshots

//first byte of binary websocket frames
#define WEB_BIN_FFT      0x01
#define WEB_BIN_AUDIO    0x02
#define WEB_BIN_CONSOLE  0x03

//websocket opcodes
#define WS_OP_TEXT       0x1
#define WS_OP_BINARY     0x2

//----------------------------------------------------------------------------
// JSON parser and builder (dsd_web_json.c)
//----------------------------------------------------------------------------

typedef enum { JV_NULL = 0, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_type;

typedef struct jv
{
  jv_type t;
  int b;
  double num;
  char * str;         //string value (JV_STR) or raw number text (JV_NUM)
  char * key;         //member name when inside an object
  struct jv * child;  //first element/member
  struct jv * next;   //next sibling
} jv;

jv * jv_parse (const char * s, size_t len);
void jv_free (jv * v);
jv * jv_get (jv * obj, const char * key);
jv * jv_at (jv * arr, int index);
jv * jv_first (jv * arr); //first element, iterate with ->next
int jv_count (jv * arr);
const char * jv_str (jv * obj, const char * key, const char * def);
double jv_num (jv * obj, const char * key, double def);
long long jv_int (jv * obj, const char * key, long long def);
int jv_bool (jv * obj, const char * key, int def);
int jv_has (jv * obj, const char * key);

typedef struct
{
  char * buf;
  size_t len;
  size_t cap;
  int depth;
  uint8_t first[32]; //first element flag per nesting level
} jb;

void jb_init (jb * b);
void jb_free (jb * b);
void jb_reset (jb * b);
void jb_raw (jb * b, const char * s);
void jb_rawn (jb * b, const char * s, size_t n);
void jb_printf (jb * b, const char * fmt, ...);
void jb_obj (jb * b, const char * key);   //begin object (key may be NULL)
void jb_obj_end (jb * b);
void jb_arr (jb * b, const char * key);   //begin array (key may be NULL)
void jb_arr_end (jb * b);
void jb_key (jb * b, const char * key);
void jb_str (jb * b, const char * key, const char * s);
void jb_strn (jb * b, const char * key, const char * s, size_t n);
void jb_int (jb * b, const char * key, long long v);
void jb_uint (jb * b, const char * key, unsigned long long v);
void jb_dbl (jb * b, const char * key, double v, int prec);
void jb_bool (jb * b, const char * key, int v);
void jb_null (jb * b, const char * key);
void jb_hex (jb * b, const char * key, unsigned long long v, int width); //hex as string
void jb_escape (jb * b, const char * s, size_t n); //append escaped string body (valid UTF-8 output)

//----------------------------------------------------------------------------
// Server (dsd_web_server.c)
//----------------------------------------------------------------------------

//embedded static files (generated at build time from the web/ directory)
typedef struct
{
  const char * path;
  const char * mime;
  const unsigned char * data;
  unsigned int len;
} web_asset;

extern const web_asset web_assets[];
extern const unsigned int web_assets_count;

int  web_server_start (const char * bind_addr, int port, const char * root_dir, const char * data_dir, const char * password);
void web_server_set_area (const char * name, const char * path); //download area name -> directory
void web_server_stop (void);
int  web_server_clients (void);
uint32_t web_server_subs (void); //union of all client subscriptions
void web_broadcast (int kind, int opcode, const void * data, size_t len, uint32_t need_sub);
void web_send (int client_id, int kind, int opcode, const void * data, size_t len);
int  web_pop_command (int * client_id, char ** json);
void web_set_subs (int client_id, uint32_t subs);
int  web_client_authed (int client_id);
void web_set_authed (int client_id, int authed);
const char * web_session_token (void);
int  web_path_component_ok (const char * s);

//----------------------------------------------------------------------------
// Spectrum (dsd_spectrum.c)
//----------------------------------------------------------------------------

typedef struct
{
  int bins;              //number of bins in power[]
  uint64_t center;       //hardware center frequency (Hz) of the frame
  uint32_t rate;         //sample rate (span) in Hz
  uint64_t seq;          //frame sequence number
  double data_time;      //wall time (s) when the newest sample in the frame arrived
  float * power;         //linear power per bin, low to high frequency
} spectrum_frame;

void spectrum_start (void);
void spectrum_stop (void);
void spectrum_push_iq (const int16_t * iq, int n, uint32_t center, uint32_t rate, int rotated); //from RTL demod thread
void spectrum_configure (int fft_size, int fps, int averaging);
void spectrum_get_config (int * fft_size, int * fps, int * averaging);
int  spectrum_copy_latest (spectrum_frame * out, int max_bins); //returns 1 if a frame was copied
double spectrum_now (void);

//----------------------------------------------------------------------------
// Bookmarks (dsd_web_bookmarks.c), decoder thread only
//----------------------------------------------------------------------------

#define WEB_BM_MAX 1000

typedef struct
{
  int id;
  long long freq;  //Hz
  char name[96];
  char group[48];
  char mode[16];   //decoder key (see web_mode_valid), "" keeps the current decoder
  char mod[8];     //"c4fm" / "qpsk" / "gfsk", "" keeps the current modulation
} web_bookmark;

void bm_init (const char * path); //load the list, NULL or "" = default location
const char * bm_path (void);
int  bm_get (int id, web_bookmark * out);
int  bm_near (long long freq, long long tol); //id of the closest bookmark within tol Hz, 0 = none
int  bm_add (const web_bookmark * bm);       //add (no duplicate per channel), returns the id
void bm_send (int client); //client < 0 sends to everybody
int  bm_command (int client, const char * cmd, jv * j); //returns 1 if cmd was a bookmark command

//----------------------------------------------------------------------------
// Activity log and system picture (dsd_web_data.c), decoder thread only
//----------------------------------------------------------------------------

void web_act_init (dsd_opts * opts, dsd_state * state);
void web_act_poll (dsd_opts * opts, dsd_state * state); //track calls, broadcast new items
void web_act_send (int client);                         //everything kept, for a browser that just connected
void web_act_clear (void);
void web_act_retuned (void);                            //new frequency: forget the P25 site picture
void web_enc_name (dsd_state * state, int slot, char * out, size_t n); //encryption on a slot's call, "" if clear
const char * web_call_gps (int slot); //GPS sent during the slot's current call, "" if none
int  web_offer_bp_key (int key); //a Basic Privacy key found in packet data; 1 if Auto-find took it (dsd_web.c)
void web_trunk_json (jb * b);    //inside the status "trunk" object: the followed call, the last one, tallies, skips
int  web_trunk_skip (dsd_opts * opts, dsd_state * state); //block the followed call until it ends (the caller returns to the CC)
void web_tg_mode (long long id, const char * mode);       //"A" follow / "B" don't follow a talkgroup or radio
void web_tg_name (long long id, const char * name);
void web_groups_reset (void);                    //a group CSV import replaced state->group_array
int  web_p25_cc_list (long long * out, int max); //the site's control channel, then its secondary ones

//----------------------------------------------------------------------------
// Main thread interface (dsd_web.c)
//----------------------------------------------------------------------------

int  web_mode_valid (const char * key); //decoder keys accepted by the "mode" command and bookmarks
void web_init (dsd_opts * opts, dsd_state * state);
void web_poll (dsd_opts * opts, dsd_state * state);
void web_shutdown (dsd_opts * opts, dsd_state * state);
void web_audio_push (dsd_opts * opts, const void * data, size_t nbytes, int analog);
void web_event_pushed (Event_History_I * event_struct);
void web_symbol_tap (int symbol);

#ifdef __cplusplus
}
#endif

#endif //DSD_WEB_H

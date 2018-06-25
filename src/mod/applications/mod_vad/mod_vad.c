/*
 *  mod_vad - detects human speech
 */

#include <switch.h>
#include "webrtc/common_audio/vad/include/webrtc_vad.h"
#include "webrtc/common_audio/vad/vad_filterbank.h"
#include <sys/time.h>

#define USER_SAID_EVENT "user_said"
#define START_SPEECH_EVENT "start_speech"
#define STOP_SPEECH_EVENT "stop_speech"

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load);
SWITCH_STANDARD_APP(vad_start_function);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown);
SWITCH_MODULE_DEFINITION(mod_vad, mod_vad_load, mod_vad_shutdown, NULL);

static switch_bool_t vad_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);

typedef struct{
	void* data;
	uint32_t datalen;
	uint32_t rate;
	uint32_t samples;
	int is_voiced;
	int16_t total_energy;
	int16_t log_energy;

} audio_frame_t;

typedef struct{
	 audio_frame_t ** buffer;
     int size;
     int capacity;
} ring_buffer_t;

typedef struct vad_session_info {
	switch_core_session_t *session;
	VadInst* vad;
	ring_buffer_t* ring_buffer;
	ring_buffer_t* wav_buffer;
	int triggered;
	long long last_start_speech_milliseconds;
	long long last_stop_speech_milliseconds;

} vad_session_info_t;

struct wavfile_header {
	char	riff_tag[4];
	int		riff_length;
	char	wave_tag[4];
	char	fmt_tag[4];
	int		fmt_length;
	short	audio_format;
	short	num_channels;
	int		sample_rate;
	int		byte_rate;
	short	block_align;
	short	bits_per_sample;
	char	data_tag[4];
	int		data_length;
};

long long current_timestamp();
void vad_collector( vad_session_info_t *vad_info, switch_frame_t *frame);
void fire_user_said_event( vad_session_info_t *vad_info, const char* file_name);
void fire_start_speech_event( vad_session_info_t *vad_info);
void fire_stop_speech_event( vad_session_info_t *vad_info);
void init_ring_buffer(ring_buffer_t* rb, int capacity);
void push_to_ring_buffer(ring_buffer_t* rb, audio_frame_t * data);
void clear_ring_buffer(ring_buffer_t* rb);
void free_ring_buffer(ring_buffer_t* rb);
void handle_non_triggered( vad_session_info_t *vad_info);
void handle_triggered( vad_session_info_t *vad_info);
int count_frames_in_ring_buffer( ring_buffer_t* rb, int is_voice);
int is_enough_frames_in_ring_buffer( ring_buffer_t* rb, int frames_count);
void copy_ring_buffer_to_wav_buffer( vad_session_info_t *vad_info);
void copy_frame_to_buffer( vad_session_info_t *vad_info, switch_frame_t *frame, ring_buffer_t* rb);
void write_wav_buffer_to_file( vad_session_info_t *vad_info);
FILE * wav_file_open( const char *file_name );
void wav_file_close( FILE *file );
void RB_print( ring_buffer_t* rb);
int16_t get_median( ring_buffer_t *rb);

static struct {
	char *user_said_directory;
	int ring_buffer_size;
	int int_ring_buffer_full_percentage;
	float float_ring_buffer_full_percentage;
	int wav_buffer_size;
	int aggressive_mode;
	int after_stop_speech_timeout_ms;
	int debug_log_energy;
	int high_log_energy_level;
	int low_log_energy_level;
	int debug_log_energy_low_level;

} globals;

static switch_xml_config_int_options_t config_opt_ring_buffer_size = { SWITCH_TRUE, 1, SWITCH_TRUE, 10000 };
static switch_xml_config_int_options_t config_opt_ring_buffer_full_percentage = { SWITCH_TRUE, 0, SWITCH_TRUE, 100 };
static switch_xml_config_int_options_t config_opt_wav_buffer_size = { SWITCH_TRUE, 1, SWITCH_TRUE, 10000 };
static switch_xml_config_int_options_t config_opt_aggressive_mode = { SWITCH_TRUE, 0, SWITCH_TRUE, 3 };
static switch_xml_config_int_options_t config_opt_after_stop_speech_timeout_ms = { SWITCH_TRUE, 0, SWITCH_TRUE, 10000 };
static switch_xml_config_int_options_t config_opt_debug_log_energy = { SWITCH_TRUE, 0, SWITCH_TRUE, 1 };
static switch_xml_config_int_options_t config_opt_log_energy_level = { SWITCH_TRUE, 0, SWITCH_TRUE, 100000 };

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM( "user_said_directory", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.user_said_directory, "/tmp", NULL, NULL, NULL),
	SWITCH_CONFIG_ITEM( "ring_buffer_size", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.ring_buffer_size, (void *) 16, &config_opt_ring_buffer_size, NULL, NULL),
	SWITCH_CONFIG_ITEM( "ring_buffer_full_percentage", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.int_ring_buffer_full_percentage, (void *) 90, &config_opt_ring_buffer_full_percentage, NULL, NULL),
	SWITCH_CONFIG_ITEM( "wav_buffer_size", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.wav_buffer_size, (void *) 1000, &config_opt_wav_buffer_size, NULL, NULL),
	SWITCH_CONFIG_ITEM( "aggressive_mode", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.aggressive_mode, (void *) 0, &config_opt_aggressive_mode, NULL, NULL),
	SWITCH_CONFIG_ITEM( "after_stop_speech_timeout_ms", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.after_stop_speech_timeout_ms, (void *) 2000, &config_opt_after_stop_speech_timeout_ms, NULL, NULL),
	SWITCH_CONFIG_ITEM( "debug_log_energy", SWITCH_CONFIG_BOOL, CONFIG_RELOADABLE, &globals.debug_log_energy, (void *) 0, &config_opt_debug_log_energy, NULL, NULL),
	SWITCH_CONFIG_ITEM( "high_log_energy_level", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.high_log_energy_level, (void *) 1400, &config_opt_log_energy_level, NULL, NULL),
	SWITCH_CONFIG_ITEM( "low_log_energy_level", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.low_log_energy_level, (void *) 800, &config_opt_log_energy_level, NULL, NULL),
	SWITCH_CONFIG_ITEM( "debug_log_energy_low_level", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.debug_log_energy_low_level, (void *) 0, &config_opt_log_energy_level, NULL, NULL),
	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config(switch_bool_t reload)
{
	memset(&globals, 0, sizeof(globals));

	if (switch_xml_config_parse_module_settings("vad.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open vad.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	globals.float_ring_buffer_full_percentage = globals.int_ring_buffer_full_percentage / 100.0;

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load)
{
	switch_application_interface_t *app_interface;

	if (switch_event_reserve_subclass(USER_SAID_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", USER_SAID_EVENT);
		return SWITCH_STATUS_TERM;
	}

	if (switch_event_reserve_subclass(START_SPEECH_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", START_SPEECH_EVENT);
		return SWITCH_STATUS_TERM;
	}

	if (switch_event_reserve_subclass(STOP_SPEECH_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", STOP_SPEECH_EVENT);
		return SWITCH_STATUS_TERM;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VAD enabled\n");

	do_config( SWITCH_FALSE);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "user_said_directory: %s\n", globals.user_said_directory);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ring_buffer_size: %d\n", globals.ring_buffer_size);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ring_buffer_full_percentage: %d\n", globals.int_ring_buffer_full_percentage);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "wav_buffer_size: %d\n", globals.wav_buffer_size);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "aggressive_mode: %d\n", globals.aggressive_mode);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "after_stop_speech_timeout_ms: %d\n", globals.after_stop_speech_timeout_ms);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "debug_log_energy: %d\n", globals.debug_log_energy);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "high_log_energy_level: %d\n", globals.high_log_energy_level);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "low_log_energy_level: %d\n", globals.low_log_energy_level);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "debug_log_energy_low_level: %d\n", globals.debug_log_energy_low_level);

	SWITCH_ADD_APP(app_interface, "vad", "VAD", "Detect human speech", vad_start_function, "[start] [stop]", SAF_NONE);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(vad_start_function)
{
	vad_session_info_t *vad_info;
	switch_status_t status;
	switch_channel_t *channel;
	switch_media_bug_t *bug;

	channel = switch_core_session_get_channel(session);

	/* Is this channel already set? */
	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_vad_");
	/* If yes */
	if (bug != NULL) {
		/* If we have a stop remove audio bug */
		if (strcasecmp(data, "stop") == 0) {
			switch_channel_set_private(channel, "_vad_", NULL);
			switch_core_media_bug_remove(session, &bug);
			return;
		}

		/* We have already started */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run 2 at once on the same channel!\n");

		return;
	}

	vad_info = (vad_session_info_t *) switch_core_session_alloc(session, sizeof(vad_session_info_t));
	vad_info->session = session;

	status = switch_core_media_bug_add( session, "vad", NULL, vad_callback, vad_info, 0, SMBF_READ_REPLACE, &bug);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failure hooking to stream\n");
		return;
	}

	do_config( SWITCH_TRUE);

	switch_channel_set_private(channel, "_vad_", bug);
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown)
{
	switch_event_free_subclass(USER_SAID_EVENT);
	switch_event_free_subclass(START_SPEECH_EVENT);
	switch_event_free_subclass(STOP_SPEECH_EVENT);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VAD disabled\n");

	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t vad_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	vad_session_info_t *vad_info;
	switch_frame_t *frame;

	vad_info = (vad_session_info_t *) user_data;
	if (vad_info == NULL) {
		return SWITCH_FALSE;
	}

	switch (type) {

		case SWITCH_ABC_TYPE_INIT:
			vad_info->vad = WebRtcVad_Create();
			WebRtcVad_Init( vad_info->vad);
			WebRtcVad_set_mode( vad_info->vad, globals.aggressive_mode);

			vad_info->ring_buffer = (ring_buffer_t*)(malloc(sizeof(ring_buffer_t)));
			init_ring_buffer( vad_info->ring_buffer, globals.ring_buffer_size);

			vad_info->wav_buffer = (ring_buffer_t*)(malloc(sizeof(ring_buffer_t)));
			init_ring_buffer( vad_info->wav_buffer, globals.wav_buffer_size);

			break;

		case SWITCH_ABC_TYPE_CLOSE:
			WebRtcVad_Free( vad_info->vad);
			free_ring_buffer( vad_info->ring_buffer);
			free_ring_buffer( vad_info->wav_buffer);
			break;

		case SWITCH_ABC_TYPE_READ_REPLACE:
			frame = switch_core_media_bug_get_read_replace_frame(bug);
			vad_collector( vad_info, frame);

		default:
			break;
		}

	return SWITCH_TRUE;
}

long long current_timestamp()
{
    struct timeval te;
    long long milliseconds;

    gettimeofday(&te, NULL);
    milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;

    return milliseconds;
}

void vad_collector( vad_session_info_t *vad_info, switch_frame_t *frame)
{
	//uint32_t energy = 0;
	//int tot_rshifts = 0;
	long long dif_cur_and_stop = 0;
	//int16_t total_energy = 0;
	//int16_t log_energy = 0;

	//energy = (uint32_t) WebRtcSpl_Energy((int16_t*) frame->data, frame->datalen, &tot_rshifts);
	//printf( "energy = %u\n", energy);

	if( vad_info->triggered)
		copy_frame_to_buffer( vad_info, frame, vad_info->wav_buffer);

	copy_frame_to_buffer( vad_info, frame, vad_info->ring_buffer);
	//RB_print( vad_info->ring_buffer);
	vad_info->triggered ? handle_triggered( vad_info) : handle_non_triggered( vad_info);

	if( vad_info->last_stop_speech_milliseconds){
		dif_cur_and_stop = current_timestamp() - vad_info->last_stop_speech_milliseconds;
		//printf("dif_cur_and_stop : %lld\n", dif_cur_and_stop);
	}

	if( dif_cur_and_stop  > globals.after_stop_speech_timeout_ms && vad_info->last_stop_speech_milliseconds - vad_info->last_start_speech_milliseconds > 0){
		vad_info->last_start_speech_milliseconds = 0;
		vad_info->last_stop_speech_milliseconds = 0;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop speech\n");
		fire_stop_speech_event( vad_info);
		write_wav_buffer_to_file( vad_info);
		clear_ring_buffer( vad_info->wav_buffer);
	}
}

void RB_print( ring_buffer_t * rb)
{
	printf( "RB %p: \n", (void*) rb);
	for( int i = 0; i < rb->size; i++)
	{
		audio_frame_t* frame = rb->buffer[ i];
		//printf( "%p ", (void*)frame);
		//printf( "%d ", frame->is_voiced);
		printf( "%u ", frame->log_energy);
	}
	printf( "\n");
}

// copy_frame_to_buffer -> copy_frame_to_buffer + analyze_frame

void copy_frame_to_buffer( vad_session_info_t *vad_info, switch_frame_t *frame, ring_buffer_t* rb)
{
	audio_frame_t* audio_frame = (audio_frame_t*)(malloc(sizeof(audio_frame_t)));
	audio_frame->datalen = frame->datalen;
	audio_frame->rate = frame->rate;
	audio_frame->samples = frame->samples;
	audio_frame->data = (void*)(malloc(sizeof(char) * frame->datalen));
	audio_frame->total_energy = 0;
	memcpy( audio_frame->data, frame->data, frame->datalen);
	audio_frame->is_voiced = WebRtcVad_Process(vad_info->vad, audio_frame->rate, audio_frame->data, audio_frame->samples);
	LogOfEnergy( (int16_t*) audio_frame->data, audio_frame->datalen / sizeof( int16_t), 0, &(audio_frame->total_energy), &(audio_frame->log_energy));
	//printf( "new! %p: %d\n", (void*)frame, audio_frame->is_voiced);
	push_to_ring_buffer( rb, audio_frame);
}

void handle_non_triggered( vad_session_info_t *vad_info)
{
	int num_voiced;
	int16_t median;
	num_voiced = count_frames_in_ring_buffer( vad_info->ring_buffer, 1);
	//printf( "vad_info->ring_buffer:%p, num_voiced:%d\n", (void*)vad_info->ring_buffer, num_voiced);
	median = get_median( vad_info->ring_buffer);

	if( globals.debug_log_energy == SWITCH_TRUE && median > globals.debug_log_energy_low_level)
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "silence, median: %u\n", median);

	if( is_enough_frames_in_ring_buffer( vad_info->ring_buffer, num_voiced) && median > globals.high_log_energy_level)
	{
		vad_info->last_start_speech_milliseconds = current_timestamp();
		vad_info->triggered = 1;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "start speech\n");
		fire_start_speech_event( vad_info);
		copy_ring_buffer_to_wav_buffer( vad_info);
		clear_ring_buffer( vad_info->ring_buffer);
	}
}

void handle_triggered( vad_session_info_t *vad_info)
{
	int num_unvoiced;
	int16_t median;
	num_unvoiced = count_frames_in_ring_buffer( vad_info->ring_buffer, 0);
	median = get_median( vad_info->ring_buffer);

	if( globals.debug_log_energy == SWITCH_TRUE && median > globals.debug_log_energy_low_level)
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "voice, median: %u\n", median);

	if( is_enough_frames_in_ring_buffer( vad_info->ring_buffer, num_unvoiced) || median < globals.low_log_energy_level)
	{
		vad_info->last_stop_speech_milliseconds = current_timestamp();
		vad_info->triggered = 0;
		clear_ring_buffer( vad_info->ring_buffer);
	}
}

int16_t get_median( ring_buffer_t *rb)
{
	int16_t temp;
    int i, j;

    const int size = rb->size;
    int16_t log_energy_array[ size];

    //printf( "source log_energy_array:\n");
    for( i = 0; i < size; i++){
    	log_energy_array[ i] = rb->buffer[ i]->log_energy;
    	//printf( "%u ", log_energy_array[ i]);
	}
    //printf( "\n");


    // the following two loops sort the array x in ascending order
    for( i = 0; i < size - 1; i++) {
        for( j = i + 1; j < size; j++) {
            if( log_energy_array[ j] < log_energy_array[ i])
            {
                // swap elements
                temp = log_energy_array[ i];
                log_energy_array[ i] = log_energy_array[j];
                log_energy_array[ j] = temp;
            }
        }
    }

    //printf( "sorted log_energy_array:\n");
	//for( i = 0; i < size; i++){
	//	printf( "%u ", log_energy_array[ i]);
	//}
	//printf( "\n\n");

    if( size % 2 == 0)
    {
        // if there is an even number of elements, return mean of the two elements in the middle
        return( ( log_energy_array[ size / 2] + log_energy_array[ size / 2 - 1]) / 2);
    }
    else
    {
        // else return the element in the middle
        return log_energy_array[ size / 2];
    }
}

void fire_user_said_event( vad_session_info_t *vad_info, const char* file_name)
{
	switch_event_t *event;

	switch_event_create_subclass( &event, SWITCH_EVENT_CUSTOM, USER_SAID_EVENT);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "file_path", file_name);
	switch_core_session_queue_event( vad_info->session, &event);
}

void fire_start_speech_event( vad_session_info_t *vad_info)
{
	switch_event_t *event;

	switch_event_create_subclass( &event, SWITCH_EVENT_CUSTOM, START_SPEECH_EVENT);
	switch_core_session_queue_event( vad_info->session, &event);
}

void fire_stop_speech_event( vad_session_info_t *vad_info)
{
	switch_event_t *event;

	switch_event_create_subclass( &event, SWITCH_EVENT_CUSTOM, STOP_SPEECH_EVENT);
	switch_core_session_queue_event( vad_info->session, &event);
}

int count_frames_in_ring_buffer( ring_buffer_t* rb, int is_voice)
{
	int count;
	count = 0;
	for( int i = 0; i < rb->size; i++)
	{
		audio_frame_t* frame = rb->buffer[ i];
		if( is_voice == frame->is_voiced)
			count++;
	}

	return count;
}

int is_enough_frames_in_ring_buffer( ring_buffer_t* rb, int frames_count)
{
	return ( frames_count > globals.float_ring_buffer_full_percentage * rb->capacity);
}

void init_ring_buffer(ring_buffer_t* rb, int capacity)
{
	rb->buffer = (audio_frame_t**)(malloc(sizeof(audio_frame_t) * capacity));
	rb->size = 0;
	rb->capacity = capacity;
}

void push_to_ring_buffer(ring_buffer_t* rb, audio_frame_t * data)
{
	if( rb->size < rb->capacity){
		rb->buffer[ rb->size++] = data;
		return;
	}

	if( rb->size == rb->capacity)
	{
		audio_frame_t * frame;
		frame = rb->buffer[ 0];
		free( frame->data);
		free( frame);

		for( int i = 0; i < rb->size - 1; i++)
		{
			rb->buffer[ i] = rb->buffer[ i + 1];
		}

		rb->buffer[ rb->size - 1] = data;
	}
}

void clear_ring_buffer(ring_buffer_t* rb)
{
	for( int i = 0; i < rb->size; i++)
	{
		audio_frame_t * frame = rb->buffer[ i];
		free( frame->data);
		free( frame);
	}
	free(rb->buffer);
	rb->buffer = (audio_frame_t **)(malloc(sizeof(audio_frame_t *) * rb->capacity));
	rb->size = 0;
}

void free_ring_buffer(ring_buffer_t* rb)
{
	for( int i = 0; i < rb->size; i++)
	{
		audio_frame_t * frame = rb->buffer[ i];
		free( frame->data);
		free( frame);
	}
	free(rb->buffer);
}

void copy_ring_buffer_to_wav_buffer( vad_session_info_t *vad_info)
{
	for( int i = 0; i < vad_info->ring_buffer->size; i++)
	{
		audio_frame_t* frame = vad_info->ring_buffer->buffer[ i];
		audio_frame_t* audio_frame = (audio_frame_t*)(malloc(sizeof(audio_frame_t)));
		audio_frame->datalen = frame->datalen;
		audio_frame->rate = frame->rate;
		audio_frame->samples = frame->samples;
		audio_frame->data = (void*)(malloc(sizeof(char) * frame->datalen));
		memcpy( audio_frame->data, frame->data, frame->datalen);
		audio_frame->is_voiced = frame->is_voiced;
		push_to_ring_buffer( vad_info->wav_buffer, audio_frame);
	}
}

void write_wav_buffer_to_file( vad_session_info_t *vad_info)
{
	char file_name[ 80];
	FILE* file;

	char unique_filename[] = "XXXXXX";
	mktemp( unique_filename);

	if( !strcmp( unique_filename, "")){
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mktemp failed\n");
		return;
	}

	strcpy( file_name, globals.user_said_directory);
	strcat( file_name, "/");
	strcat( file_name, unique_filename);
	strcat( file_name, ".wav");

	file = wav_file_open( file_name);

	if( !file){
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "no such directory %s\n", file_name);
		return;
	}

	for( int i = 0; i < vad_info->wav_buffer->size; i++)
	{
		audio_frame_t* frame = vad_info->wav_buffer->buffer[ i];
		fwrite( frame->data, sizeof( char), frame->datalen, file);
	}

	wav_file_close( file);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "write %d frames to file %s\n", vad_info->wav_buffer->size, file_name);
	fire_user_said_event( vad_info, file_name);
}

FILE * wav_file_open( const char *file_name )
{
	struct wavfile_header header;
	FILE * file;

	int samples_per_second = 8000;
	int bits_per_sample = 16;

	strncpy(header.riff_tag,"RIFF",4);
	strncpy(header.wave_tag,"WAVE",4);
	strncpy(header.fmt_tag,"fmt ",4);
	strncpy(header.data_tag,"data",4);

	header.riff_length = 0;
	header.fmt_length = 16;
	header.audio_format = 1;
	header.num_channels = 1;
	header.sample_rate = samples_per_second;
	header.byte_rate = samples_per_second*(bits_per_sample/8);
	header.block_align = bits_per_sample/8;
	header.bits_per_sample = bits_per_sample;
	header.data_length = 0;

	file = fopen(file_name,"wbx");
	if(!file) return 0;

	fwrite(&header,sizeof(header),1,file);

	fflush(file);

	return file;
}

void wav_file_close( FILE *file )
{
	int riff_length;
	int file_length = ftell(file);

	int data_length = file_length - sizeof(struct wavfile_header);
	fseek(file,sizeof(struct wavfile_header) - sizeof(int),SEEK_SET);
	fwrite(&data_length,sizeof(data_length),1,file);

	riff_length = file_length - 8;
	fseek(file,4,SEEK_SET);
	fwrite(&riff_length,sizeof(riff_length),1,file);

	fclose(file);
}

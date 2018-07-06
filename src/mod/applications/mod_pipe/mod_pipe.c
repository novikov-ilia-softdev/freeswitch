/*
 *  mod_pipe - send audio frame to pipe
 */

#include <switch.h>
#include <sys/time.h>

typedef struct {
	switch_core_session_t *session;
	char* pipe_path;
	int pipe_fd;
	pthread_t open_pipe_thread;
	int is_opened;
	pthread_mutex_t is_opened_mutex;

} pipe_session_info_t;

#define START_WRITE_TO_PIPE_EVENT "start_write_to_pipe"
#define PIPE_PATH "pipe_path"

SWITCH_MODULE_LOAD_FUNCTION( mod_pipe_load);
SWITCH_STANDARD_APP( mod_pipe_start_function);
SWITCH_MODULE_SHUTDOWN_FUNCTION( mod_pipe_shutdown);
SWITCH_MODULE_DEFINITION(mod_pipe, mod_pipe_load, mod_pipe_shutdown, NULL);

static switch_bool_t get_audio_frame_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
void handle_session_init( pipe_session_info_t *session);
void handle_session_close( pipe_session_info_t *session);
void handle_frame( pipe_session_info_t *session, switch_frame_t *frame);
void* open_pipe_thread( void *arg);
void fire_start_write_to_pipe_event( pipe_session_info_t *pipe_session);

static struct {
	char *use_pipe_path;
	char *pipe_dir;

} globals;

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM( "use_pipe_path", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.use_pipe_path, "", NULL, NULL, NULL),
	SWITCH_CONFIG_ITEM( "pipe_dir", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.pipe_dir, "", NULL, NULL, NULL),
	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config(switch_bool_t reload)
{
	memset(&globals, 0, sizeof(globals));

	if (switch_xml_config_parse_module_settings("pipe.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open pipe.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_pipe_load)
{
	switch_application_interface_t *app_interface;

	if (switch_event_reserve_subclass(START_WRITE_TO_PIPE_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", START_WRITE_TO_PIPE_EVENT);
		return SWITCH_STATUS_TERM;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe enabled\n");

	do_config( SWITCH_FALSE);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "use_pipe_path: %s\n", globals.use_pipe_path);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_dir: %s\n", globals.pipe_dir);

	SWITCH_ADD_APP(app_interface, "pipe", "pipe", "send audio frame to pipe", mod_pipe_start_function, "[start] [stop]", SAF_NONE);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(mod_pipe_start_function)
{
	pipe_session_info_t *pipe_info;
	switch_status_t status;
	switch_channel_t *channel;
	switch_media_bug_t *bug;

	channel = switch_core_session_get_channel(session);

	/* Is this channel already set? */
	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_pipe_");
	/* If yes */
	if (bug != NULL) {
		/* If we have a stop remove audio bug */
		if (strcasecmp(data, "stop") == 0) {
			switch_channel_set_private(channel, "_pipe_", NULL);
			switch_core_media_bug_remove(session, &bug);
			return;
		}

		/* We have already started */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run 2 at once on the same channel!\n");

		return;
	}

	pipe_info = (pipe_session_info_t *) switch_core_session_alloc(session, sizeof(pipe_session_info_t));
	pipe_info->session = session;

	status = switch_core_media_bug_add( session, "pipe", NULL, get_audio_frame_callback, pipe_info, 0, SMBF_READ_REPLACE, &bug);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failure hooking to stream\n");
		return;
	}

	do_config( SWITCH_TRUE);

	switch_channel_set_private(channel, "_pipe_", bug);
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_pipe_shutdown)
{
	switch_event_free_subclass(START_WRITE_TO_PIPE_EVENT);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe disabled\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t get_audio_frame_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	pipe_session_info_t *session;
	switch_frame_t *frame;

	session = (pipe_session_info_t *) user_data;
	if (session == NULL) {
		return SWITCH_FALSE;
	}

	switch (type) {

		case SWITCH_ABC_TYPE_INIT:
			handle_session_init( session);
			break;

		case SWITCH_ABC_TYPE_CLOSE:
			handle_session_close( session);
			break;

		case SWITCH_ABC_TYPE_READ_REPLACE:
			frame = switch_core_media_bug_get_read_replace_frame(bug);
			handle_frame( session, frame);
			break;

		default:
			break;
		}

	return SWITCH_TRUE;
}

void handle_session_init( pipe_session_info_t *session)
{
	char* file_path_template;

	session->pipe_path = (char *)(malloc(80));

	if( !strcmp( globals.use_pipe_path, ""))
	{
		file_path_template = "XXXXXX";
		strcpy( session->pipe_path, globals.pipe_dir);
		strcat( session->pipe_path, file_path_template);
		mktemp( session->pipe_path);
	}
	else
	{
		strcpy( session->pipe_path, globals.use_pipe_path);
	}

	if( !strcmp( session->pipe_path, "") || !session->pipe_path)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mktemp error!\n");
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "pipe_path %s\n", session->pipe_path);

	if( mkfifo( session->pipe_path, 0666) == -1)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mkfifo error!\n");
		return;
	}

	fire_start_write_to_pipe_event( session);

	session->is_opened = 0;
	if (pthread_mutex_init(&session->is_opened_mutex, NULL) != 0)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mutex init failed\n");
		return;
	}

	if( pthread_create( &session->open_pipe_thread, NULL, open_pipe_thread, session))
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "pthread_create error!\n");
		return;
	}
}

void* open_pipe_thread( void *arg)
{
	pipe_session_info_t * session = (pipe_session_info_t *) arg;
	session->pipe_fd = open( session->pipe_path, O_WRONLY);
	if( session->pipe_fd == -1)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open error!\n");
		return 0;
	}

	pthread_mutex_lock(&session->is_opened_mutex);
	session->is_opened = 1;
	pthread_mutex_unlock(&session->is_opened_mutex);

	return 0;
}

void handle_session_close( pipe_session_info_t *session)
{
	pthread_mutex_lock(&session->is_opened_mutex);
	if( session->is_opened)
	{
		close( session->pipe_fd);
	}
	else
	{
		pthread_cancel( session->open_pipe_thread);
	}
	unlink( session->pipe_path);
	free( session->pipe_path);
	pthread_mutex_unlock(&session->is_opened_mutex);
	pthread_mutex_destroy(&session->is_opened_mutex);
}

void handle_frame( pipe_session_info_t *session, switch_frame_t *frame)
{
	pthread_mutex_lock(&session->is_opened_mutex);
	if( session->is_opened && session->pipe_fd != -1)
		write( session->pipe_fd, frame->data, frame->datalen);
	pthread_mutex_unlock(&session->is_opened_mutex);
}

void fire_start_write_to_pipe_event( pipe_session_info_t *pipe_session)
{
	switch_event_t *event;

	switch_event_create_subclass( &event, SWITCH_EVENT_CUSTOM, START_WRITE_TO_PIPE_EVENT);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, PIPE_PATH, pipe_session->pipe_path);
	switch_core_session_queue_event( pipe_session->session, &event);
}

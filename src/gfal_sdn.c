#include <glib.h>
#include <gfal_plugins_api.h>
#include <utils/gfal_uri.h>
#include <string.h>

/**
 * Structure used to hold the different pairs notified by gfal2
 */
typedef struct {
    char *source, *destination;
} pair_t;

typedef struct {
    GSequence *pairs;
    gfal2_context_t context;
    off_t total_size;
} sdn_t;


/**
 * Create a pair from the description sent by gfal2, which is of the form
 * source => destination
 * Note that source and destination are xml-escaped
 */
pair_t* sdn_create_pair(const char* description)
{
    gchar** splitted = g_strsplit(description, " => ", 2);

    pair_t* pair = g_new0(pair_t, 1);
    pair->source = g_strdup(splitted[0]);
    pair->destination = g_strdup(splitted[1]);

    g_strfreev(splitted);

    return pair;
}


/**
 * Frees the memory used by a pair
 */
void sdn_release_pair(gpointer data)
{
    pair_t* pair = (pair_t*)data;
    g_free(pair->source);
    g_free(pair->destination);
    g_free(pair);
}


/**
 * Creates and initializes a sdn_t
 */
sdn_t* sdn_create_data(gfal2_context_t context)
{
    sdn_t* sdn = g_new0(sdn_t, 1);
    sdn->pairs = g_sequence_new(sdn_release_pair);
    sdn->context = context;
    return sdn;
}


/**
 * Frees the memory used by a sdn_t
 */
void sdn_release_data(gpointer p)
{
    sdn_t* sdn = (sdn_t*)p;
    g_sequence_free(sdn->pairs);
    g_free(sdn);
}


/**
 * Stat the source and add its file size to the total_size
 */
void sdn_add_size(gpointer data, gpointer udata)
{
    sdn_t* sdn = (sdn_t*)udata;
    pair_t* pair = (pair_t*)data;

    GError* error = NULL;
    struct stat st;
    if (gfal2_stat(sdn->context, pair->source, &st, &error) < 0) {
        gfal2_log(G_LOG_LEVEL_ERROR, "Could not stat %s (%s)", pair->source, error->message);
        g_error_free(error);
    }
    else {
        sdn->total_size += st.st_size;
    }
}


/**
 * This is the "heart" of the plugin
 * When this enters, it has the list of files that will be transferred
 */
void sdn_notify_remote(sdn_t* data)
{
    gfal2_uri source, destination;

    // Get the hostname and port
    GSequenceIter* iter = g_sequence_get_begin_iter(data->pairs);
    if (!iter)
        return;

    pair_t* pair = (pair_t*)g_sequence_get(iter);
    gfal2_parse_uri(pair->source, &source, NULL);
    gfal2_parse_uri(pair->destination, &destination, NULL);

    // Calculate the size
    data->total_size = 0;
    g_sequence_foreach(data->pairs, sdn_add_size, data);

    gfal2_log(G_LOG_LEVEL_WARNING, "Between %s and %s %d files with a total size of %lld bytes",
            source.domain, destination.domain, g_sequence_get_length(data->pairs),
            (long long)data->total_size);

    // PLACEHOLDER
    // Build the message and submit to the SDN
}


/**
 * This method is called by gfal2 and plugins
 */
void sdn_event_listener(const gfalt_event_t e, gpointer user_data)
{
    sdn_t* data = (sdn_t*)user_data;

    if (e->stage ==  GFAL_EVENT_LIST_ENTER) {
        g_sequence_remove_range(
                g_sequence_get_begin_iter(data->pairs),
                g_sequence_get_end_iter(data->pairs));
    }
    else if (e->stage == GFAL_EVENT_LIST_ITEM) {
        g_sequence_append(data->pairs, sdn_create_pair(e->description));
    }
    else if (e->stage == GFAL_EVENT_LIST_EXIT) {
        sdn_notify_remote(data);
    }
}


/**
 * This method will be called when a copy method is called (bulk or single method).
 * The SDN plugin can take this chance to inject its own event listener into the copy configuration.
 * Several listeners can be registered at the same time, so this is safe.
 */
int sdn_copy_enter_hook(plugin_handle plugin_data, gfal2_context_t context,
        gfalt_params_t params, GError** error)
{
    GError* tmp_error = NULL;

    // Register the new listener
    // If there was one already, it will be released
    gfalt_add_event_callback(params, sdn_event_listener,
            sdn_create_data(context), sdn_release_data,
            &tmp_error);

    if (tmp_error) {
        gfal2_propagate_prefixed_error(error, tmp_error, __func__);
        return -1;
    }

    gfal2_log(G_LOG_LEVEL_MESSAGE, "SDN event listener registered");

    return 0;
}

/**
 * Return the plugin name
 */
const char* sdn_get_name()
{
    return "SDN";
}

/**
 * This method is called by gfal2 when a context is instantiated
 */
gfal_plugin_interface gfal_plugin_init(gfal2_context_t handle, GError** err)
{
    gfal_plugin_interface sdn_plugin;
    memset(&sdn_plugin, 0, sizeof(gfal_plugin_interface));

    sdn_plugin.getName = sdn_get_name;
    sdn_plugin.copy_enter_hook = sdn_copy_enter_hook;

    return sdn_plugin;
}

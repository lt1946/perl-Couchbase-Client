#include "perl-couchbase.h"
#include <assert.h>

/** Complete callback checks for data and populates it,
 * data callback will invoke a callback */

static void call_to_perl(PLCB_couch_handle_t *handle, int cbidx, SV *datasv, AV *statusav)
{
    SV **tmpsv;
    
    dSP;
    
    tmpsv = av_fetch(statusav, cbidx, 0);
    if (!tmpsv) {
        die("Couldn't invoke callback (none installed)");
    }
    
    ENTER;
    SAVETMPS;
    
    /**
     * callback($handle, $status, $data);
     */

    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newRV_inc(SvRV(handle->self_rv))));
    XPUSHs(sv_2mortal(newRV_inc((SV*)statusav)));
    XPUSHs(sv_2mortal(datasv));
    PUTBACK;
    
    call_sv(*tmpsv, G_DISCARD);
    
    FREETMPS;
    LEAVE;
}

#define MAYBE_STOP_LOOP(handle) \
    if ((handle->flags & PLCB_COUCHREQf_STOPITER) \
            && (handle->flags & PLCB_COUCHREQf_STOPITER_NOOP) == 0) { \
        \
        handle->parent->io_ops->stop_event_loop(handle->parent->io_ops); \
        handle->flags &= ~(PLCB_COUCHREQf_STOPITER); \
        handle->flags |= PLCB_COUCHREQf_STOPITER_NOOP; \
    }

static
void data_callback(libcouchbase_couch_request_t couchreq,
                   libcouchbase_t instance,
                   const void *cookie,
                   libcouchbase_error_t error,
                   libcouchbase_http_status_t status,
                   const char *path, libcouchbase_size_t npath,
                   const void *bytes, libcouchbase_size_t nbytes)
{
    PLCB_couch_handle_t *handle = (PLCB_couch_handle_t*)cookie;
    SV **tmpsv;
    SV *datasv;

    if ((handle->flags & PLCB_COUCHREQf_INITIALIZED) == 0) {
        handle->flags |= PLCB_COUCHREQf_INITIALIZED;
        tmpsv = av_fetch(handle->plpriv, PLCB_COUCHIDX_HTTP, 1);
        sv_setiv(*tmpsv, status);
    }
    
    if (nbytes) {
        datasv = newSVpv((const char*)bytes, nbytes);
    } else {
        datasv = &PL_sv_undef;
    }


    if (error != LIBCOUCHBASE_SUCCESS) {
        plcb_ret_set_err(handle->parent, handle->plpriv, error);
        libcouchbase_cancel_couch_request(couchreq);
        handle->lcb_request = NULL;
        handle->flags |=
                (PLCB_COUCHREQf_TERMINATED |
                PLCB_COUCHREQf_ERROR |
                PLCB_COUCHREQf_STOPITER);

        call_to_perl(handle, PLCB_COUCHIDX_CALLBACK_COMPLETE, datasv, handle->plpriv);
    } else {
        call_to_perl(handle, PLCB_COUCHIDX_CALLBACK_DATA, datasv, handle->plpriv);
    }
    
    /* The callback might have enough data to stop the iteration */
    MAYBE_STOP_LOOP(handle);
}

/**
 * This is called when the HTTP response is complete. We only call out to
 * Perl if the request is chunked. Otherwise we reduce the overhead by
 * simply appending data.
 */
static
void complete_callback(libcouchbase_couch_request_t couchreq,
                       libcouchbase_t instance,
                       const void *cookie,
                       libcouchbase_error_t error,
                       libcouchbase_http_status_t status,
                       const char *path, libcouchbase_size_t npath,
                       const void *bytes, libcouchbase_size_t nbytes)
{
    PLCB_couch_handle_t *handle = (PLCB_couch_handle_t*)cookie;
    handle->flags |= PLCB_COUCHREQf_TERMINATED;
    handle->lcb_request = NULL;

    if (error != LIBCOUCHBASE_SUCCESS || (status < 200 && status > 299)) {
        handle->flags |= PLCB_COUCHREQf_ERROR;
    }
    plcb_ret_set_err(handle->parent, handle->plpriv, error);

    if ( (handle->flags & PLCB_COUCHREQf_CHUNKED) == 0) {
        sv_setiv(*( av_fetch(handle->plpriv, PLCB_COUCHIDX_HTTP, 1) ), status);

        if (nbytes) {
            SV *datasv;
            datasv = *(av_fetch(handle->plpriv, PLCB_RETIDX_VALUE, 1));
            sv_setpvn(datasv, (const char*)bytes, nbytes);
        }

        /*set HTTP code */
        handle->parent->io_ops->stop_event_loop(handle->parent->io_ops);

    } else {
        /* chunked */

        /* If the chunked mode has errored prematurely, this is where we get the
         * information from..
         */
        sv_setiv(*( av_fetch(handle->plpriv, PLCB_COUCHIDX_HTTP, 1)), status);

        call_to_perl(handle, PLCB_COUCHIDX_CALLBACK_COMPLETE,
                     &PL_sv_undef, handle->plpriv);
        handle->parent->io_ops->stop_event_loop(handle->parent->io_ops);
    }
    MAYBE_STOP_LOOP(handle);
}

SV* plcb_couch_handle_new(HV *stash, SV *cbo_sv, PLCB_t *cbo)
{
    SV *my_iv;
    SV *blessed_rv;
    SV *av_rv;
    AV *retav;
    PLCB_couch_handle_t *newhandle;
    
    assert(stash);
    Newxz(newhandle, 1, PLCB_couch_handle_t);
    
    my_iv = newSViv(PTR2IV(newhandle));
    blessed_rv = newRV_noinc(my_iv);
    sv_bless(blessed_rv, stash);
    SvREFCNT_inc(stash);
    newhandle->self_rv = newRV_inc(my_iv);
    sv_rvweaken(newhandle->self_rv);
    
    retav = newAV();
    av_store(retav, PLCB_COUCHIDX_CBO, newRV_inc(SvRV(cbo_sv)));
    av_store(retav, PLCB_COUCHIDX_HTTP, newSViv(-1));

    newhandle->parent = cbo;
    newhandle->plpriv = retav;
    av_rv = newRV_inc((SV*)retav);
    sv_bless(av_rv, cbo->couch.handle_av_stash);
    return blessed_rv;
}

void plcb_couch_handle_free(PLCB_couch_handle_t *handle)
{
    if (handle->plpriv) {
        SvREFCNT_dec(handle->plpriv);
        handle->plpriv = NULL;
    }
    if (handle->self_rv) {
        SvREFCNT_dec(handle->self_rv);
        handle->self_rv = NULL;
    }
    Safefree(handle);
}

void plcb_couch_handle_finish(PLCB_couch_handle_t *handle)
{
    if (handle->flags & PLCB_COUCHREQf_TERMINATED) {
        /* already stopped */
        return;
    }

    if ( (handle->flags & PLCB_COUCHREQf_ACTIVE) == 0) {
        return;
    }
    if (handle->lcb_request) {
        libcouchbase_cancel_couch_request(handle->lcb_request);
    }
    handle->flags |= PLCB_COUCHREQf_TERMINATED;
}

/**
 * This invokes a non-chunked request, and waits until all data has
 * arrived. This is less complex and more performant than the incremental
 * variant
 */

void plcb_couch_handle_execute_all(PLCB_couch_handle_t *handle,
                                   libcouchbase_http_method_t method,
                                   const char *path, size_t npath,
                                   const char *body, size_t nbody)
{
    libcouchbase_error_t err;
    handle->lcb_request =
            libcouchbase_make_couch_request(handle->parent->instance,
                                          handle,
                                          path, npath,
                                          body, nbody,
                                          method,
                                          0,
                                          &err);
    handle->flags = 0;
    if (err != LIBCOUCHBASE_SUCCESS) {
        warn("Got error!!!");
        plcb_ret_set_err(handle->parent, handle->plpriv, err);
        handle->flags |= (PLCB_COUCHREQf_TERMINATED|PLCB_COUCHREQf_ERROR);
        return;
    }
    
    handle->flags |= PLCB_COUCHREQf_ACTIVE;
    handle->parent->io_ops->run_event_loop(handle->parent->io_ops);
}

/**
 * Initializes a handle for chunked/iterative invocation
 */

void plcb_couch_handle_execute_chunked_init(PLCB_couch_handle_t *handle,
                                            libcouchbase_http_method_t method,
                                            const char *path, size_t npath,
                                            const char *body, size_t nbody)
{
    libcouchbase_error_t err;
    handle->flags = PLCB_COUCHREQf_CHUNKED;
    
    handle->lcb_request =
            libcouchbase_make_couch_request(handle->parent->instance,
                                          handle,
                                          path, npath,
                                          body, nbody,
                                          method,
                                          1,
                                          &err);
    if (err != LIBCOUCHBASE_SUCCESS) {
        /* Pretend we're done, and call the data callback */
        plcb_ret_set_err(handle->parent, handle->plpriv, err);
        call_to_perl(handle, PLCB_COUCHIDX_CALLBACK_COMPLETE,
                     &PL_sv_undef, handle->plpriv);
    }
    
    handle->flags |= PLCB_COUCHREQf_ACTIVE;
}

/**
 * Loops until a callback terminates the current step.
 * This also does some sanity checking and will not invoke the event loop.
 *
 * Returns true if we did something, or 0 if we couldn't
 */

int plcb_couch_handle_execute_chunked_step(PLCB_couch_handle_t *handle)
{
    if (handle->flags & PLCB_COUCHREQf_TERMINATED) {
        return 0;
    }

    handle->flags &= ~(PLCB_COUCHREQf_STOPITER|PLCB_COUCHREQf_STOPITER_NOOP);

    handle->parent->io_ops->run_event_loop(handle->parent->io_ops);
    /* Returned? */
    return ((handle->flags & PLCB_COUCHREQf_TERMINATED) == 0);
}

void plcb_couch_callbacks_setup(PLCB_t *object)
{
    libcouchbase_set_couch_data_callback(object->instance,
                                         data_callback);
    libcouchbase_set_couch_complete_callback(object->instance,
                                             complete_callback);
}
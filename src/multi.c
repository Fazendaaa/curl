#include "curl-common.h"
#include <time.h>

#if LIBCURL_VERSION_MAJOR > 7 || (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 28)
#define HAS_MULTI_WAIT 1
#endif

#if LIBCURL_VERSION_MAJOR > 7 || (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 30)
#define HAS_CURLMOPT_MAX_TOTAL_CONNECTIONS 1
#endif

multiref *get_multiref(SEXP ptr){
  multiref *mref = (multiref*) R_ExternalPtrAddr(ptr);
  if(!mref)
    Rf_error("multiref pointer is dead");
  return mref;
}

void multi_release(reference *ref){
  /* Release the easy-handle */
  CURL *handle = ref->handle;
  CURLM *multi = ref->async.mref->m;
  massert(curl_multi_remove_handle(multi, handle));
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);

  /* Unprotect R callbacks */
  R_ReleaseObject(ref->async.complete);
  R_ReleaseObject(ref->async.error);
  R_ReleaseObject(ref->handleptr);

  /* Remove the node from its multihandle request-list */
  refnode_remove(ref);

  /* Reset multi state struct */
  if(ref->async.content.buf){
    free(ref->async.content.buf);
    ref->async.content.buf = NULL;
    ref->async.content.size = 0;
  }
  ref->async.mref = NULL;
  ref->async.content.buf = NULL;
  ref->async.content.size = 0;
  ref->async.complete = NULL;
  ref->async.error = NULL;
  ref->async.node = NULL;

  /* Unlock handle (and cleanup if needed) */
  ref->locked = 0;
  ref->refCount--;
  clean_handle(ref);
}

SEXP R_multi_cancel(SEXP handle_ptr){
  reference *ref = get_ref(handle_ptr);
  if(ref->async.mref)
    multi_release(ref);
  return handle_ptr;
}

SEXP R_multi_add(SEXP handle_ptr, SEXP cb_complete, SEXP cb_error, SEXP pool_ptr){
  multiref *mref = get_multiref(pool_ptr);
  CURLM *multi = mref->m;

  reference *ref = get_ref(handle_ptr);
  if(ref->locked)
    Rf_error("Handle is locked. Probably in use in a connection or async request.");

  /* placeholder body */
  curl_easy_setopt(ref->handle, CURLOPT_WRITEFUNCTION, append_buffer);
  curl_easy_setopt(ref->handle, CURLOPT_WRITEDATA, &(ref->async.content));

  /* add to scheduler */
  massert(curl_multi_add_handle(multi, ref->handle));

  /* create node in ref */
  ref->async.mref = mref;
  mref->list = refnode_add(mref->list, ref);

  /* set multi callbacks */
  R_PreserveObject(ref->async.complete = cb_complete);
  R_PreserveObject(ref->async.error = cb_error);
  R_PreserveObject(ref->handleptr);

  /* lock and protect handle */
  ref->refCount++;
  ref->locked = 1;
  return handle_ptr;
}

SEXP R_multi_run(SEXP pool_ptr, SEXP timeout){
  multiref *mref = get_multiref(pool_ptr);
  CURLM *multi = mref->m;

  int total_pending = 0;
  int total_success = 0;
  int total_fail = 0;
  int dirty = 0;
  double time_max = asReal(timeout);

  clock_t time_start = clock();
  double seconds_elapsed = 0;
  do {
    dirty = 0;
    if(pending_interrupt())
      break;
    /* Required by old versions of libcurl */
    CURLMcode res = CURLM_CALL_MULTI_PERFORM;
    while(res == CURLM_CALL_MULTI_PERFORM)
      res = curl_multi_perform(multi, &(total_pending));

    /* check for multi errors */
    if(res != CURLM_OK)
      break;

    /* check for completed requests */
    int msgq = 0;
    do {
      CURLMsg *m = curl_multi_info_read(multi, &msgq);
      if(m && (m->msg == CURLMSG_DONE)){
        dirty = 1;
        reference *ref = NULL;
        CURL *handle = m->easy_handle;
        CURLcode status = m->data.result;
        assert(curl_easy_getinfo(handle, CURLINFO_PRIVATE, (char**) &ref));

        // prepare for callback
        SEXP cb_complete = PROTECT(ref->async.complete);
        SEXP cb_error = PROTECT(ref->async.error);
        SEXP buf = PROTECT(allocVector(RAWSXP, ref->async.content.size));
        if(ref->async.content.buf && ref->async.content.size)
          memcpy(RAW(buf), ref->async.content.buf, ref->async.content.size);

        //release handle for use by callbacks
        multi_release(ref);

        // callbacks must be trycatch! we should continue the loop
        if(status == CURLE_OK){
          total_success++;
          if(Rf_isFunction(cb_complete)){
            int ok;
            int arglen = Rf_length(FORMALS(cb_complete));
            //if(arglen == 1 && TAG(FORMALS(cb_complete)) == R_DotsSymbol) arglen = 99;
            SEXP out = PROTECT(make_handle_response(ref));
            SET_VECTOR_ELT(out, 5, buf);
            SEXP call = PROTECT(LCONS(cb_complete, arglen ? LCONS(out, R_NilValue) : R_NilValue));
            R_tryEval(call, R_GlobalEnv, &ok);
            UNPROTECT(2);
          }
        } else {
          total_fail++;
          if(Rf_isFunction(cb_error)){
            int ok;
            int arglen = Rf_length(FORMALS(cb_complete));
            //if(arglen == 1 && TAG(FORMALS(cb_complete)) == R_DotsSymbol) arglen = 99;
            SEXP buf = PROTECT(mkString(curl_easy_strerror(status)));
            SEXP call = PROTECT(LCONS(cb_error, arglen ? LCONS(buf, R_NilValue) : R_NilValue));
            R_tryEval(call, R_GlobalEnv, &ok);
            UNPROTECT(2);
          }
        }

        // watch out: ref/handle might be modified by callback functions!
        UNPROTECT(3);

        // trick to break from outer loop on interrupt
        if(pending_interrupt()){
          time_max = 0;
          break;
        }
      }
    } while (msgq > 0);

    /* check for timeout */
    if(time_max > 0){
      seconds_elapsed = (double) (clock() - time_start) / CLOCKS_PER_SEC;
      if(seconds_elapsed > time_max)
        break;
    }
  } while((dirty || total_pending) && time_max);

  SEXP res = PROTECT(allocVector(VECSXP, 3));
  SET_VECTOR_ELT(res, 0, ScalarInteger(total_success));
  SET_VECTOR_ELT(res, 1, ScalarInteger(total_fail));
  SET_VECTOR_ELT(res, 2, ScalarInteger(total_pending));

  SEXP names = PROTECT(allocVector(STRSXP, 3));
  SET_STRING_ELT(names, 0, mkChar("success"));
  SET_STRING_ELT(names, 1, mkChar("error"));
  SET_STRING_ELT(names, 2, mkChar("pending"));
  setAttrib(res, R_NamesSymbol, names);
  UNPROTECT(2);
  return res;
}

void fin_multi(SEXP ptr){
  multiref *mref = get_multiref(ptr);
  while(mref->list->ref)
    multi_release(mref->list->ref);
  curl_multi_cleanup(mref->m);
  free(mref->list);
  free(mref);
  R_ClearExternalPtr(ptr);
}

SEXP R_multi_new(){
  multiref *ref = calloc(1, sizeof(multiref));
  ref->m = curl_multi_init();
  ref->list = refnode_init();
  SEXP ptr = PROTECT(R_MakeExternalPtr(ref, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, fin_multi, 1);
  setAttrib(ptr, R_ClassSymbol, mkString("curl_multi"));
  UNPROTECT(1);
  return ptr;
}

SEXP R_multi_setopt(SEXP pool_ptr, SEXP total_con, SEXP host_con, SEXP multiplex){
  multiref *mref = get_multiref(pool_ptr);
  CURLM *multi = mref->m;
  #ifdef CURLPIPE_MULTIPLEX
    if(asLogical(multiplex))
      massert(curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX));
  #endif

  #ifdef HAS_CURLMOPT_MAX_TOTAL_CONNECTIONS
    massert(curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long) asInteger(total_con)));
    massert(curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long) asInteger(host_con)));
  #endif
  return pool_ptr;
}

SEXP R_multi_list(SEXP pool_ptr){
  multiref *mref = get_multiref(pool_ptr);
  int len = 0;
  struct refnode * node = mref->list;
  while((node = node->prev))
    len++;
  SEXP out = PROTECT(allocVector(VECSXP, len));
  node = mref->list;
  for(int i = 0; i < len; i++){
    SET_VECTOR_ELT(out, i, node->ref->handleptr);
    node = node->prev;
  }
  UNPROTECT(1);
  return out;
}

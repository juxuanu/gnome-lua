/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * search.h: The search results of the overview: the interface search
 * providers implement, the results the providers hand back, the widgets that
 * show them, and the view that runs a search over all providers. Port of
 * js/ui/search.js (and of the `Highlighter` class of js/misc/util.js).
 *
 * The JS protocol was duck-typed: a provider was any object with
 * `getInitialResultSet ()`, `getSubsearchResultSet ()`, `getResultMetas ()`,
 * `activateResult ()`, `launchSearch ()`, `filterResults ()`,
 * `createResultObject ()` and the fields `id`, `appInfo`, `canLaunchSearch`,
 * `isRemoteProvider` and `maxResults`. It is the #ShellSearchProvider
 * interface now. JS classes implement it with
 * `Implements: [Shell.SearchProvider]`, override the properties with
 * `GObject.ParamSpec.override ('id', Shell.SearchProvider)` and so on, and
 * implement `vfunc_get_initial_result_set ()` and the other virtual
 * functions. The asynchronous ones get a #ShellSearchRequest to answer
 * (`request.return_results ()`, `request.return_metas ()`) instead of
 * returning a promise.
 *
 * The metadata of a result (`{id, name, description, createIcon,
 * clipboardText}`) is a #ShellSearchResultMeta.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "st.h"

#include "ui/icon-grid.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellHighlighter                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_HIGHLIGHTER (shell_highlighter_get_type ())
G_DECLARE_FINAL_TYPE (ShellHighlighter, shell_highlighter,
                      SHELL, HIGHLIGHTER, GObject)

ShellHighlighter *shell_highlighter_new       (const char * const *terms);
char             *shell_highlighter_highlight (ShellHighlighter *self,
                                               const char       *text);

/* ------------------------------------------------------------------------ */
/* ShellSearchResultMeta                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_RESULT_META (shell_search_result_meta_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchResultMeta, shell_search_result_meta,
                      SHELL, SEARCH_RESULT_META, GObject)

/**
 * ShellSearchResultMetaCreateIconFunc:
 * @size: the size of the icon in logical pixels
 * @user_data: (closure): the data given to
 *   shell_search_result_meta_set_create_icon_func ()
 *
 * Makes the icon (or thumbnail) of a result (`createIcon ()`).
 *
 * Returns: (transfer full) (nullable): the icon actor, or %NULL if the
 *   result has no icon
 */
typedef ClutterActor * (* ShellSearchResultMetaCreateIconFunc) (int      size,
                                                                gpointer user_data);

ShellSearchResultMeta *shell_search_result_meta_new                  (const char *id,
                                                                      const char *name);

const char            *shell_search_result_meta_get_id               (ShellSearchResultMeta *self);
const char            *shell_search_result_meta_get_name             (ShellSearchResultMeta *self);
const char            *shell_search_result_meta_get_description      (ShellSearchResultMeta *self);
void                   shell_search_result_meta_set_description      (ShellSearchResultMeta *self,
                                                                      const char            *description);
const char            *shell_search_result_meta_get_clipboard_text   (ShellSearchResultMeta *self);
void                   shell_search_result_meta_set_clipboard_text   (ShellSearchResultMeta *self,
                                                                      const char            *clipboard_text);

void                   shell_search_result_meta_set_create_icon_func (ShellSearchResultMeta                *self,
                                                                      ShellSearchResultMetaCreateIconFunc   func,
                                                                      gpointer                              user_data,
                                                                      GDestroyNotify                        destroy);
ClutterActor          *shell_search_result_meta_create_icon          (ShellSearchResultMeta *self,
                                                                      int                    size);

/* ------------------------------------------------------------------------ */
/* ShellSearchRequest                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_REQUEST (shell_search_request_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchRequest, shell_search_request,
                      SHELL, SEARCH_REQUEST, GObject)

GCancellable *shell_search_request_get_cancellable (ShellSearchRequest *self);
void          shell_search_request_return_results  (ShellSearchRequest  *self,
                                                    const char * const  *results);
void          shell_search_request_return_metas    (ShellSearchRequest   *self,
                                                    ShellSearchResultMeta **metas,
                                                    gsize                  n_metas);
void          shell_search_request_return_error    (ShellSearchRequest *self,
                                                    const char         *message);
void          shell_search_request_return_gerror   (ShellSearchRequest *self,
                                                    GError             *error);

/* ------------------------------------------------------------------------ */
/* ShellSearchProvider                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_PROVIDER (shell_search_provider_get_type ())
G_DECLARE_INTERFACE (ShellSearchProvider, shell_search_provider,
                     SHELL, SEARCH_PROVIDER, GObject)

/**
 * ShellSearchProviderInterface:
 * @parent_iface: the parent interface
 *
 * The interface of the sources of search results (the applications provider,
 * the D-Bus providers of installed applications, and what extensions add to
 * the search).
 *
 * The properties are read only: #ShellSearchProvider:id is the identifier of
 * the provider, #ShellSearchProvider:app-info the application the provider
 * searches in (a provider without one shows its results in a grid, a provider
 * with one in a list headed by the application),
 * #ShellSearchProvider:can-launch-search whether `launch_search` does
 * something, #ShellSearchProvider:is-remote-provider whether the provider is a
 * D-Bus provider, which the view reloads when the installed applications or
 * the search settings change, and #ShellSearchProvider:max-results the
 * maximum number of results a grid shows (-1 for as many as fit).
 */
struct _ShellSearchProviderInterface
{
  GTypeInterface parent_iface;

  /**
   * ShellSearchProviderInterface::get_initial_result_set:
   * @self: the provider
   * @terms: (array zero-terminated=1): the search terms
   * @cancellable: (nullable): cancelled when the terms change
   * @request: the request to answer with shell_search_request_return_results ()
   *
   * Starts a search for @terms (`getInitialResultSet ()`). The arguments are
   * only valid during the call. The default implementation answers with an
   * error.
   */
  void            (* get_initial_result_set)   (ShellSearchProvider  *self,
                                                const char * const   *terms,
                                                GCancellable         *cancellable,
                                                ShellSearchRequest   *request);

  /**
   * ShellSearchProviderInterface::get_subsearch_result_set:
   * @self: the provider
   * @previous_results: (array zero-terminated=1): the results of the previous search
   * @terms: (array zero-terminated=1): the search terms, which narrow those of the
   *   previous search
   * @cancellable: (nullable): cancelled when the terms change
   * @request: the request to answer with shell_search_request_return_results ()
   *
   * Starts a search which narrows the previous one
   * (`getSubsearchResultSet ()`). The arguments are only valid during the
   * call. The default implementation answers with an error.
   */
  void            (* get_subsearch_result_set) (ShellSearchProvider  *self,
                                                const char * const   *previous_results,
                                                const char * const   *terms,
                                                GCancellable         *cancellable,
                                                ShellSearchRequest   *request);

  /**
   * ShellSearchProviderInterface::get_result_metas:
   * @self: the provider
   * @ids: (array zero-terminated=1): the identifiers of the results
   * @cancellable: (nullable): cancelled when the results are not needed any more
   * @request: the request to answer with shell_search_request_return_metas (),
   *   with one meta for each identifier, in the same order
   *
   * Asks for the metadata used to show results (`getResultMetas ()`). The
   * arguments are only valid during the call. The default implementation
   * answers with an error.
   */
  void            (* get_result_metas)         (ShellSearchProvider  *self,
                                                const char * const   *ids,
                                                GCancellable         *cancellable,
                                                ShellSearchRequest   *request);

  /**
   * ShellSearchProviderInterface::activate_result:
   * @self: the provider
   * @id: the identifier of the result
   * @terms: (array zero-terminated=1): the search terms
   *
   * The user activated the result @id found by a search for @terms
   * (`activateResult ()`). The default implementation does nothing.
   */
  void            (* activate_result)          (ShellSearchProvider  *self,
                                                const char           *id,
                                                const char * const   *terms);

  /**
   * ShellSearchProviderInterface::launch_search:
   * @self: the provider
   * @terms: (array zero-terminated=1): the search terms
   *
   * The user wants to see all the results of a search for @terms in the
   * application of the provider (`launchSearch ()`). The default
   * implementation does nothing.
   */
  void            (* launch_search)            (ShellSearchProvider  *self,
                                                const char * const   *terms);

  /**
   * ShellSearchProviderInterface::filter_results:
   * @self: the provider
   * @results: (array zero-terminated=1): the identifiers of the results
   * @max_number: the number of results to show
   *
   * Limits @results to what is shown (`filterResults ()`). The default
   * implementation keeps the first @max_number results.
   *
   * Returns: (transfer full) (array zero-terminated=1): the results to show
   */
  char **         (* filter_results)           (ShellSearchProvider  *self,
                                                const char * const   *results,
                                                int                   max_number);

  /**
   * ShellSearchProviderInterface::create_result_object:
   * @self: the provider
   * @meta: the meta of the result
   *
   * Makes the actor that shows a result (`createResultObject ()`). The
   * default implementation returns %NULL.
   *
   * Returns: (transfer full) (nullable): the actor, or %NULL to use the
   *   default result of the section of the provider
   */
  ClutterActor *  (* create_result_object)     (ShellSearchProvider  *self,
                                                ShellSearchResultMeta *meta);
};

char         *shell_search_provider_get_id                (ShellSearchProvider *self);
GAppInfo     *shell_search_provider_get_app_info          (ShellSearchProvider *self);
gboolean      shell_search_provider_get_can_launch_search (ShellSearchProvider *self);
gboolean      shell_search_provider_get_is_remote_provider (ShellSearchProvider *self);
int           shell_search_provider_get_max_results       (ShellSearchProvider *self);

void          shell_search_provider_get_initial_result_set_async   (ShellSearchProvider *self,
                                                                    const char * const  *terms,
                                                                    GCancellable        *cancellable,
                                                                    GAsyncReadyCallback  callback,
                                                                    gpointer             user_data);
char        **shell_search_provider_get_initial_result_set_finish  (ShellSearchProvider *self,
                                                                    GAsyncResult        *result,
                                                                    GError             **error);
void          shell_search_provider_get_subsearch_result_set_async (ShellSearchProvider *self,
                                                                    const char * const  *previous_results,
                                                                    const char * const  *terms,
                                                                    GCancellable        *cancellable,
                                                                    GAsyncReadyCallback  callback,
                                                                    gpointer             user_data);
char        **shell_search_provider_get_subsearch_result_set_finish (ShellSearchProvider *self,
                                                                     GAsyncResult        *result,
                                                                     GError             **error);
void          shell_search_provider_get_result_metas_async         (ShellSearchProvider *self,
                                                                    const char * const  *ids,
                                                                    GCancellable        *cancellable,
                                                                    GAsyncReadyCallback  callback,
                                                                    gpointer             user_data);
ShellSearchResultMeta **shell_search_provider_get_result_metas_finish (ShellSearchProvider *self,
                                                                       GAsyncResult        *result,
                                                                       gsize               *n_metas,
                                                                       GError             **error);

void          shell_search_provider_activate_result        (ShellSearchProvider *self,
                                                            const char          *id,
                                                            const char * const  *terms);
void          shell_search_provider_launch_search          (ShellSearchProvider *self,
                                                            const char * const  *terms);
char        **shell_search_provider_filter_results         (ShellSearchProvider *self,
                                                            const char * const  *results,
                                                            int                  max_number);
ClutterActor *shell_search_provider_create_result_object   (ShellSearchProvider  *self,
                                                            ShellSearchResultMeta *meta);

/* ------------------------------------------------------------------------ */
/* ShellSearchEntry                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_ENTRY (shell_search_entry_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchEntry, shell_search_entry,
                      SHELL, SEARCH_ENTRY, StEntry)

/* ------------------------------------------------------------------------ */
/* ShellSearchResultsView                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_RESULTS_VIEW (shell_search_results_view_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchResultsView, shell_search_results_view,
                      SHELL, SEARCH_RESULTS_VIEW, StBoxLayout)

ShellSearchResultsView *shell_search_results_view_new (ShellSearchProvider *application_provider);

void           shell_search_results_view_register_provider   (ShellSearchResultsView *self,
                                                              ShellSearchProvider    *provider);
void           shell_search_results_view_unregister_provider (ShellSearchResultsView *self,
                                                              ShellSearchProvider    *provider);

void           shell_search_results_view_set_terms           (ShellSearchResultsView *self,
                                                              const char * const     *terms);
char         **shell_search_results_view_get_terms           (ShellSearchResultsView *self);
gboolean       shell_search_results_view_get_search_in_progress (ShellSearchResultsView *self);

void           shell_search_results_view_activate_default    (ShellSearchResultsView *self);
void           shell_search_results_view_highlight_default   (ShellSearchResultsView *self,
                                                              gboolean                highlight);
void           shell_search_results_view_popup_menu_default  (ShellSearchResultsView *self);
void           shell_search_results_view_navigate_from_entry (ShellSearchResultsView *self,
                                                              StDirectionType         direction);
char          *shell_search_results_view_highlight_terms     (ShellSearchResultsView *self,
                                                              const char             *description);

/* ------------------------------------------------------------------------ */
/* ShellSearchResult, ShellListSearchResult, ShellGridSearchResult           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_RESULT (shell_search_result_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSearchResult, shell_search_result,
                          SHELL, SEARCH_RESULT, StButton)

/**
 * ShellSearchResultClass:
 * @parent_class: the parent class
 * @activate: activates the result (`activate ()`). The default
 *   implementation asks the provider to activate the result, copies the
 *   clipboard text of the meta if there is one and asks for the overview to
 *   be toggled. #ShellGridSearchResult subclasses in JS override it with
 *   `vfunc_activate ()`
 *
 * The base class of the results of the providers that do not make their own
 * result actors (`SearchResult`).
 */
struct _ShellSearchResultClass
{
  StButtonClass parent_class;

  void (* activate) (ShellSearchResult *self);

  /*< private >*/
  gpointer padding[8];
};

ShellSearchProvider    *shell_search_result_get_provider     (ShellSearchResult *self);
ShellSearchResultMeta  *shell_search_result_get_meta_info    (ShellSearchResult *self);
ShellSearchResultsView *shell_search_result_get_results_view (ShellSearchResult *self);
void                    shell_search_result_activate         (ShellSearchResult *self);

#define SHELL_TYPE_LIST_SEARCH_RESULT (shell_list_search_result_get_type ())
G_DECLARE_FINAL_TYPE (ShellListSearchResult, shell_list_search_result,
                      SHELL, LIST_SEARCH_RESULT, ShellSearchResult)

#define SHELL_TYPE_GRID_SEARCH_RESULT (shell_grid_search_result_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellGridSearchResult, shell_grid_search_result,
                          SHELL, GRID_SEARCH_RESULT, ShellSearchResult)

/**
 * ShellGridSearchResultClass:
 * @parent_class: the parent class
 *
 * The result shown as an icon in a grid (`GridSearchResult`).
 * `SystemActionIcon` in app-display.c extends it.
 */
struct _ShellGridSearchResultClass
{
  ShellSearchResultClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellBaseIcon *shell_grid_search_result_get_icon (ShellGridSearchResult *self);

G_END_DECLS

/* eslint camelcase: ["error", { properties: "never", allow: ["^script_"] }] */

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Scripting from 'resource:///org/gnome/shell/ui/scripting.js';

// This script tests the overview (src/ui/overview.c): it shows the overview,
// waits for it to be shown, switches to the application grid, runs a search
// and hides the overview, under fatal warnings.

export var METRICS = {};

const TIMEOUT = 10000;

const results = {
    hiddenAtStart: false,
    shown: false,
    visibleWhileShown: false,
    appGridShown: false,
    searchStarted: false,
    searchEnded: false,
    hidden: false,
};

/**
 * Resolves when @object emits @signal, rejects after a timeout.
 *
 * @param {GObject.Object} object - the emitter
 * @param {string} signal - the name of the signal
 * @returns {Promise<void>}
 */
function waitForSignal(object, signal) {
    const emitted = new Promise(resolve => {
        const id = object.connect(signal, () => {
            object.disconnect(id);
            resolve();
        });
    });
    const timeout = Scripting.sleep(TIMEOUT).then(() => {
        throw new Error(`Timeout waiting for ${signal}`);
    });

    return Promise.race([emitted, timeout]);
}

/**
 * Resolves when @predicate is true, polling, rejects after a timeout.
 *
 * @param {Function} predicate - the condition to wait for
 * @param {string} description - what is waited for
 * @returns {Promise<void>}
 */
async function waitFor(predicate, description) {
    const start = Date.now();

    /* eslint-disable no-await-in-loop */
    while (!predicate()) {
        if (Date.now() - start > TIMEOUT)
            throw new Error(`Timeout waiting for ${description}`);
        await Scripting.sleep(100);
    }
    /* eslint-enable no-await-in-loop */
}

/** @returns {Promise<void>} */
export async function run() {
    console.debug('Running overview test');

    const {overview} = Main;

    /* eslint-disable no-await-in-loop */
    await Scripting.sleep(1000);
    await Scripting.waitLeisure();

    results.hiddenAtStart = !overview.visible;

    // show the overview (window picker)
    console.debug('Showing the overview');
    const shown = waitForSignal(overview, 'shown');
    overview.show();
    await shown;
    results.shown = true;
    results.visibleWhileShown = overview.visible && !overview.animation_in_progress;
    await Scripting.waitLeisure();

    // switch to the application grid
    console.debug('Showing the application grid');
    const showApps = overview.dash.show_apps_button;
    showApps.checked = true;
    await Scripting.sleep(600);
    await Scripting.waitLeisure();
    results.appGridShown = showApps.checked && overview.visible;

    // run a search
    console.debug('Searching');
    const {search_controller: searchController, search_entry: searchEntry} = overview;
    searchEntry.set_text('a');
    await waitFor(() => searchController.search_active, 'the search to start');
    results.searchStarted = true;
    await Scripting.sleep(800);
    await Scripting.waitLeisure();

    searchEntry.set_text('');
    await waitFor(() => !searchController.search_active, 'the search to end');
    results.searchEnded = true;
    await Scripting.sleep(600);
    await Scripting.waitLeisure();

    // hide the overview
    console.debug('Hiding the overview');
    const hidden = waitForSignal(overview, 'hidden');
    overview.hide();
    await hidden;
    results.hidden = !overview.visible;
    await Scripting.waitLeisure();
    /* eslint-enable no-await-in-loop */

    console.debug('Finished overview test');
}

/** @returns {void} */
export function finish() {
    if (!results.hiddenAtStart)
        throw new Error('The overview was visible at startup');

    if (!results.shown)
        throw new Error('Failed to show the overview');

    if (!results.visibleWhileShown)
        throw new Error('The overview is not visible once shown');

    if (!results.appGridShown)
        throw new Error('Failed to show the application grid');

    if (!results.searchStarted)
        throw new Error('Failed to start the search');

    if (!results.searchEnded)
        throw new Error('Failed to end the search');

    if (!results.hidden)
        throw new Error('Failed to hide the overview');
}

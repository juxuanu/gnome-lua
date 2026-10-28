import './environment.js';

import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

const loop = new GLib.MainLoop(null, false);
const id = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
    for (const name of Shell.SessionMode.list_modes())
        print(name);
    loop.quit();
});
GLib.Source.set_name_by_id(id, '[gnome-shell] listModes');
loop.run();

import GObject from 'gi://GObject';
import Shell from 'gi://Shell';

export const Indicator = GObject.registerClass(
class Indicator extends Shell.SystemIndicator {
    constructor() {
        super();

        this._indicator = this.add_indicator();
        this._indicator.icon_name = 'camera-web-symbolic';
        this._indicator.add_style_class_name('privacy-indicator');

        this._cameraMonitor = new Shell.CameraMonitor();
        this._cameraMonitor.bind_property('cameras-in-use', this._indicator,
            'visible', GObject.BindingFlags.SYNC_CREATE);
    }
});

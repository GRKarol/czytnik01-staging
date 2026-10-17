package pl.flower.reader;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

/**
 * Odbiera tekst/linki udostępnione z innych appek przez systemowe Android
 * "Udostępnij" (zob. intent-filter ACTION_SEND w AndroidManifest.xml) oraz
 * pliki książek: wysłane do Flower z komunikatora albo otwarte przez
 * "Otwórz w Flower" (ACTION_VIEW). MainActivity kopiuje plik do cache appki
 * i przekazuje tu jego ścieżkę. Cold start (appka jeszcze nie działała)
 * trafia do pending* i JS odbiera go przez getPending() przy starcie; warm
 * start (appka już działa) leci od razu jako event, bo JS ma już podpięty
 * listener.
 */
@CapacitorPlugin(name = "ShareTarget")
public class ShareTargetPlugin extends Plugin {
    private static ShareTargetPlugin instance;
    private static String pendingText;
    private static JSObject pendingFile;

    @Override
    public void load() {
        instance = this;
    }

    static void stashPending(String text) {
        pendingText = text;
    }

    static void deliverLive(String text) {
        pendingText = text;
        if (instance != null) {
            JSObject data = new JSObject();
            data.put("text", text);
            instance.notifyListeners("shareReceived", data);
        }
    }

    static void stashFile(String path, String name, String mime) {
        pendingFile = fileObject(path, name, mime);
    }

    static void deliverFileLive(String path, String name, String mime) {
        pendingFile = fileObject(path, name, mime);
        if (instance != null) {
            instance.notifyListeners("fileReceived", pendingFile);
            pendingFile = null;
        }
    }

    private static JSObject fileObject(String path, String name, String mime) {
        JSObject file = new JSObject();
        file.put("path", path);
        file.put("name", name);
        file.put("mime", mime == null ? "" : mime);
        return file;
    }

    @PluginMethod
    public void getPending(PluginCall call) {
        JSObject ret = new JSObject();
        ret.put("text", pendingText);
        if (pendingFile != null) {
            ret.put("file", pendingFile);
        }
        pendingText = null;
        pendingFile = null;
        call.resolve(ret);
    }
}

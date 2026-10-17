package pl.flower.reader;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.work.Constraints;
import androidx.work.ExistingPeriodicWorkPolicy;
import androidx.work.ExistingWorkPolicy;
import androidx.work.NetworkType;
import androidx.work.OneTimeWorkRequest;
import androidx.work.PeriodicWorkRequest;
import androidx.work.WorkManager;

import com.getcapacitor.BridgeActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.TimeUnit;

public class MainActivity extends BridgeActivity {
    private static final String UPDATE_CHECK_WORK_NAME = "flower_update_check";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(NetworkPinPlugin.class);
        registerPlugin(ShareTargetPlugin.class);
        super.onCreate(savedInstanceState);
        requestNotificationPermissionIfNeeded();
        scheduleUpdateCheck();
        handleShareIntent(getIntent(), false);
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleShareIntent(intent, true);
    }

    private void handleShareIntent(Intent intent, boolean live) {
        if (intent == null) {
            return;
        }
        // A book file from a friend: shared to Flower from a messenger or
        // mail ("Udostępnij"), or opened with it ("Otwórz w Flower").
        Uri stream = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) {
            stream = intent.getData();
        } else if (Intent.ACTION_SEND.equals(intent.getAction())) {
            stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        }
        if (stream != null) {
            handleIncomingFile(stream, intent.getType(), live);
            return;
        }
        if (!Intent.ACTION_SEND.equals(intent.getAction())) {
            return;
        }
        String type = intent.getType();
        if (type == null || !type.startsWith("text/")) {
            return;
        }
        String text = intent.getStringExtra(Intent.EXTRA_TEXT);
        if (text == null || text.trim().isEmpty()) {
            return;
        }
        if (live) {
            ShareTargetPlugin.deliverLive(text);
        } else {
            ShareTargetPlugin.stashPending(text);
        }
    }

    /** Copies the file into the app's cache; the web side picks it up there. */
    private void handleIncomingFile(Uri uri, String mime, boolean live) {
        String name = displayName(uri);
        File dir = new File(getCacheDir(), "incoming");
        if (!dir.exists() && !dir.mkdirs()) {
            return;
        }
        File target = new File(dir, System.currentTimeMillis() + "-" + name.replaceAll("[\\\\/:*?\"<>|]", "_"));
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(target)) {
            if (in == null) {
                return;
            }
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        } catch (Exception e) {
            Log.w("Flower", "incoming file failed: " + e);
            return;
        }
        String path = Uri.fromFile(target).toString();
        if (live) {
            ShareTargetPlugin.deliverFileLive(path, name, mime);
        } else {
            ShareTargetPlugin.stashFile(path, name, mime);
        }
    }

    private String displayName(Uri uri) {
        String name = null;
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(
                    uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    name = cursor.getString(0);
                }
            } catch (Exception ignored) {
                // the last path segment below
            }
        }
        if (name == null || name.trim().isEmpty()) {
            name = uri.getLastPathSegment();
        }
        return name == null || name.trim().isEmpty() ? "book" : name;
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                    this, new String[] {Manifest.permission.POST_NOTIFICATIONS}, 1001);
        }
    }

    private void scheduleUpdateCheck() {
        Constraints constraints = new Constraints.Builder()
                .setRequiredNetworkType(NetworkType.CONNECTED)
                .build();

        // PeriodicWorkRequest NIE odpala się od razu po enqueue — pierwsze
        // uruchomienie przychodzi dopiero po pełnym interwale (do 12h).
        // Bez osobnego jednorazowego requesta powiadomienie o aktualizacji
        // mogłoby się nie pojawić przez pół dnia od pierwszego uruchomienia
        // appki, co wygląda jak "to w ogóle nie działa" — dokładnie to
        // zgłosił Karol. UNIQUE + KEEP na jednorazowym też zapobiega
        // wielokrotnemu odpaleniu przy kolejnych onCreate() tej samej sesji.
        OneTimeWorkRequest immediateCheck =
                new OneTimeWorkRequest.Builder(UpdateCheckWorker.class)
                        .setConstraints(constraints)
                        .build();
        WorkManager.getInstance(this)
                .enqueueUniqueWork(
                        UPDATE_CHECK_WORK_NAME + "_immediate", ExistingWorkPolicy.KEEP, immediateCheck);

        PeriodicWorkRequest request =
                new PeriodicWorkRequest.Builder(UpdateCheckWorker.class, 12, TimeUnit.HOURS)
                        .setConstraints(constraints)
                        .build();
        WorkManager.getInstance(this)
                .enqueueUniquePeriodicWork(
                        UPDATE_CHECK_WORK_NAME, ExistingPeriodicWorkPolicy.KEEP, request);
    }
}

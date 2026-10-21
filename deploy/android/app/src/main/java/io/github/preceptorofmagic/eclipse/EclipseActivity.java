package io.github.preceptorofmagic.eclipse;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.util.SparseLongArray;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/**
 * Eclipse for Android TV.
 *
 * The whole application is the same C program as on webOS, Windows and Linux (libmain.so, started by SDL as
 * SDL_main). This activity only provides what Android keeps in Java:
 *
 * - the program's directories and device facts, published as environment variables BEFORE SDL_main starts,
 *   so the co-op peer -- an exec'd process with no Java VM -- inherits the same answers
 *   (src/app/platform/android/android_platform.h);
 * - a video SurfaceView UNDER SDL's translucent surface, which the MediaCodec module renders into: the same
 *   plane-under-the-UI model as the webOS TV (third_party/ss4s/modules/android/mediacodec.c);
 * - the display's physical mode, and switching it to the stream's refresh rate;
 * - the TV remote keys SDL does not map (colour, channel, info/guide, a remote's Menu);
 * - the Wi-Fi multicast lock mDNS host discovery needs.
 */
public class EclipseActivity extends SDLActivity {
    private static final String TAG = "Eclipse";

    /* A TvKeys number (ANDROID_SCANCODE_* - ANDROID_SCANCODE_TV_BASE in android_platform.h). */
    private static native void nativeTvKey(int key, boolean pressed);

    private static volatile EclipseActivity sInstance;
    private static volatile Surface sVideoSurface;
    private SurfaceView mVideoView;
    private int mVideoWidth, mVideoHeight;
    private WifiManager.MulticastLock mMulticastLock;

    @Override
    protected String[] getLibraries() {
        /* libmain.so's other dependencies (mbedTLS, curl) come in through its DT_NEEDED entries. */
        return new String[]{"SDL2", "main"};
    }

    /**
     * Launch extras, as the webOS launch payload: --es host_uuid UUID --ei host_app_id ID --ez coop true.
     * Parsed by app_handle_launch() in src/app/platform/android/app_android.c.
     */
    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        Intent intent = getIntent();
        if (intent != null) {
            String host = intent.getStringExtra("host_uuid");
            if (host != null && !host.isEmpty()) {
                args.add("--host-uuid");
                args.add(host);
            }
            int appId = intent.getIntExtra("host_app_id", 0);
            if (appId > 0) {
                args.add("--host-app-id");
                args.add(Integer.toString(appId));
            }
            if (intent.getBooleanExtra("coop", false)) {
                args.add("--coop");
            }
            /* Self-test builds only act on these (src/app/platform/android/selftest_android.c). */
            for (String key : new String[]{"selftest_video", "selftest_codec", "selftest_size"}) {
                String value = intent.getStringExtra(key);
                if (value != null) {
                    args.add("--" + key.replace('_', '-'));
                    args.add(value);
                }
            }
            if (intent.getBooleanExtra("selftest_peer", false)) {
                args.add("--selftest-peer");
            }
            if (intent.getBooleanExtra("selftest_audio", false)) {
                args.add("--selftest-audio");
            }
            if (intent.getBooleanExtra("selftest_display", false)) {
                args.add("--selftest-display");
            }
            if (intent.getBooleanExtra("selftest_keys", false)) {
                args.add("--selftest-keys");
            }
            String audioBackground = intent.getStringExtra("selftest_audio_background");
            if (audioBackground != null) {
                args.add("--selftest-audio-background");
                args.add(audioBackground);
            }
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        File assets = unpackAssets();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }
        publishEnvironment(assets);

        /* The video plane: below SDL's surface, which is made translucent in createSDLSurface(). */
        mVideoView = new SurfaceView(this);
        mVideoView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                sVideoSurface = holder.getSurface();
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                sVideoSurface = holder.getSurface();
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                sVideoSurface = null;
            }
        });
        mLayout.setBackgroundColor(Color.BLACK);
        mLayout.addView(mVideoView, 0, videoLayout(0, 0));

        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            mMulticastLock = wifi.createMulticastLock("Eclipse host discovery");
            mMulticastLock.setReferenceCounted(false);
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        /* Over the video surface, with an alpha channel: where the UI is transparent the stream shows through,
         * as on the TV's video plane. The EGL side asks for the alpha bits (src/app/ui/root.c). */
        surface.setZOrderMediaOverlay(true);
        surface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        return surface;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mMulticastLock != null) {
            mMulticastLock.acquire();
        }
    }

    @Override
    protected void onPause() {
        if (mMulticastLock != null && mMulticastLock.isHeld()) {
            mMulticastLock.release();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (sInstance == this) {
            sInstance = null;
        }
    }

    /* --- environment ------------------------------------------------------------------------------------ */

    private void publishEnvironment(File assets) {
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_FILES_DIR", getFilesDir().getAbsolutePath());
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_CACHE_DIR", getCacheDir().getAbsolutePath());
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_ASSETS_DIR", assets.getAbsolutePath());
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_NATIVE_DIR", getApplicationInfo().nativeLibraryDir);
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_SDK_INT", Integer.toString(Build.VERSION.SDK_INT));
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_DEVICE", Build.MANUFACTURER + " " + Build.MODEL);
        SDLActivity.nativeSetenv("ECLIPSE_ANDROID_GUIDE_IS_HOME", guideIsHome() ? "1" : "0");
    }

    /**
     * Whether the system takes a controller's Guide button before any app sees it. On the SHIELD TV the package
     * com.nvidia.shieldtech.hooks swallows the Guide (Xbox) button and presses Home instead (its
     * NvNormalizedKeylayout, 2026-10-16), and offers apps no way out, so holding View is the Guide button there
     * (session_gamepad.c). Where that package is gone or disabled, the Guide button reaches Eclipse itself.
     */
    private boolean guideIsHome() {
        try {
            return getPackageManager().getApplicationInfo("com.nvidia.shieldtech.hooks", 0).enabled;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /**
     * The packaged assets (catalogues, controller mappings, CA bundle, module list) as ordinary files, which is
     * how every other platform's C code reads them. Unpacked once per installed build into files/assets-<id>.
     */
    private File unpackAssets() {
        String build = "0";
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
            build = code + "-" + info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "No package info", e);
        }
        File dir = new File(getFilesDir(), "assets-" + build);
        File stamp = new File(dir, ".complete");
        if (stamp.exists()) {
            return dir;
        }
        File[] old = getFilesDir().listFiles();
        if (old != null) {
            for (File f : old) {
                if (f.getName().startsWith("assets-") && !f.equals(dir)) {
                    deleteTree(f);
                }
            }
        }
        try {
            copyAssetTree(getAssets(), "eclipse", dir);
            if (!stamp.createNewFile()) {
                Log.w(TAG, "Could not mark the assets complete");
            }
        } catch (IOException e) {
            Log.e(TAG, "Could not unpack the assets", e);
        }
        return dir;
    }

    private static void copyAssetTree(AssetManager am, String path, File target) throws IOException {
        String[] children = am.list(path);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("mkdir " + target);
            }
            for (String child : children) {
                copyAssetTree(am, path + "/" + child, new File(target, child));
            }
            return;
        }
        try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                deleteTree(c);
            }
        }
        if (!f.delete()) {
            Log.w(TAG, "Could not delete " + f);
        }
    }

    /* --- remote keys ------------------------------------------------------------------------------------ */

    /* THE SHIELD TV TURNS A SWITCH PRO CONTROLLER'S + INTO PLAY/PAUSE. Its window manager (handleComboKeys) logs
     * BUTTON_START and then MEDIA_PLAY_PAUSE 5 ms later for every press, held or not; no kernel device sends a Play/Pause
     * (getevent on all inputs), and an Xbox controller's Start is left alone (2026-10-22). So + never reached the stream
     * or the Start wheel. A Play/Pause from a controller with a Start button is taken as that controller's Start; one
     * that follows a real Start from the same controller within TvKeys.START_TWIN_MS is the copy and is dropped. */
    private final SparseLongArray lastRealStart = new SparseLongArray();
    private int keyLogLines;

    /** @return the event to dispatch, or null to drop it. */
    private KeyEvent controllerStart(KeyEvent event) {
        final int code = event.getKeyCode();
        final boolean controller = isControllerEvent(event);
        if (keyLogLines < 60 && (controller || code == KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)) {
            keyLogLines++;
            InputDevice device = event.getDevice();
            Log.i(TAG, "controller key " + code + " action " + event.getAction() + " repeat " + event.getRepeatCount() +
                    " device " + event.getDeviceId() + " (" + (device != null ? device.getName() : "none") + ") source 0x" +
                    Integer.toHexString(event.getSource()) + " sdl_joystick " +
                    SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId()));
        }
        final long now = SystemClock.uptimeMillis();
        if (controller && code == KeyEvent.KEYCODE_BUTTON_START) {
            lastRealStart.put(event.getDeviceId(), now);
        }
        InputDevice device = event.getDevice();
        final boolean hasStart = device != null && device.hasKeys(KeyEvent.KEYCODE_BUTTON_START)[0];
        switch (TvKeys.controllerKey(code, controller, hasStart,
                now - lastRealStart.get(event.getDeviceId(), Long.MIN_VALUE / 2))) {
            case TvKeys.CONTROLLER_KEY_DROP:
                return null;
            case TvKeys.CONTROLLER_KEY_AS_START:   /* the same event, Start's key code (changeKeyCode is not public API) */
                return new KeyEvent(event.getDownTime(), event.getEventTime(), event.getAction(),
                        KeyEvent.KEYCODE_BUTTON_START, event.getRepeatCount(), event.getMetaState(), event.getDeviceId(),
                        event.getScanCode(), event.getFlags(), event.getSource());
            default:
                return event;
        }
    }

    private static boolean isControllerEvent(KeyEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        event = controllerStart(event);
        if (event == null) {
            return true;
        }
        int tvKey = tvKeyFor(event);
        if (tvKey != 0) {
            int action = event.getAction();
            if (action == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                nativeTvKey(tvKey, true);
            } else if (action == KeyEvent.ACTION_UP) {
                nativeTvKey(tvKey, false);
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private static int tvKeyFor(KeyEvent event) {
        int source = event.getSource();
        boolean controller = (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        InputDevice device = event.getDevice();
        boolean alphabetic = device != null && device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
        return TvKeys.forKeyCode(event.getKeyCode(), controller, alphabetic);
    }

    /** Every input device, "name|sources (hex)|1 if SDL treats it as a joystick" per line: a TV remote must be a
     *  keyboard to SDL (scripts/android/patches/sdl2-2.32.10-android-tv-remote.patch). Self-test builds log it. */
    static String describeInputDevices() {
        StringBuilder out = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) {
                continue;
            }
            out.append(device.getName().replace('|', '/')).append('|')
                    .append(Integer.toHexString(device.getSources())).append('|')
                    .append(SDLControllerManager.isDeviceSDLJoystick(id) ? 1 : 0).append('\n');
        }
        return out.toString();
    }

    /* --- audio outputs ---------------------------------------------------------------------------------- */

    private static final class Outputs {
        final int[] ids;
        final String[] labels;

        Outputs(int[] ids, String[] labels) {
            this.ids = ids;
            this.labels = labels;
        }
    }

    private static Outputs audioOutputs() {
        EclipseActivity activity = sInstance;
        AudioManager am = activity != null ? (AudioManager) activity.getSystemService(Context.AUDIO_SERVICE) : null;
        AudioDeviceInfo[] devices = am != null ? am.getDevices(AudioManager.GET_DEVICES_OUTPUTS) : new AudioDeviceInfo[0];
        devices = devices.clone();
        Arrays.sort(devices, new Comparator<AudioDeviceInfo>() {
            @Override
            public int compare(AudioDeviceInfo a, AudioDeviceInfo b) {
                return Integer.compare(a.getId(), b.getId());
            }
        });
        int[] ids = new int[devices.length], types = new int[devices.length];
        CharSequence[] products = new CharSequence[devices.length];
        for (int i = 0; i < devices.length; ++i) {
            ids[i] = devices[i].getId();
            types[i] = devices[i].getType();
            products[i] = devices[i].getProductName();
        }
        return new Outputs(ids, AudioOutputs.label(types, products));
    }

    /** The outputs a stream's sound can be sent to, "id|name" per line (the audio settings, android_platform.c). */
    static String getAudioOutputs() {
        Outputs outputs = audioOutputs();
        return AudioOutputs.lines(outputs.ids, outputs.labels);
    }

    /** The current id of the output named @p name, 0 when it is not connected (the SS4S SDL module). */
    static int getAudioOutputId(String name) {
        Outputs outputs = audioOutputs();
        return AudioOutputs.idOf(outputs.ids, outputs.labels, name);
    }

    /* --- display ---------------------------------------------------------------------------------------- */

    private static Display display() {
        EclipseActivity activity = sInstance;
        if (activity == null) {
            return null;
        }
        return Build.VERSION.SDK_INT >= 30 ? activity.getDisplay() : activity.getWindowManager().getDefaultDisplay();
    }

    private static boolean displayHdr10(Display d) {
        Display.HdrCapabilities caps = d.getHdrCapabilities();
        if (caps == null) {
            return false;
        }
        for (int type : caps.getSupportedHdrTypes()) {
            if (type == Display.HdrCapabilities.HDR_TYPE_HDR10) {
                return true;
            }
        }
        return false;
    }

    /** The display's modes at its current resolution: a stream changes the refresh rate, never the size. */
    private static Display.Mode[] modesAtCurrentSize(Display d) {
        Display.Mode current = d.getMode();
        List<Display.Mode> same = new ArrayList<>();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() == current.getPhysicalWidth() &&
                    m.getPhysicalHeight() == current.getPhysicalHeight()) {
                same.add(m);
            }
        }
        return same.toArray(new Display.Mode[0]);
    }

    private static int[] ratesX100(Display.Mode[] modes) {
        int[] rates = new int[modes.length];
        for (int i = 0; i < modes.length; ++i) {
            rates[i] = Math.round(modes[i].getRefreshRate() * 100f);
        }
        return rates;
    }

    /** The refresh rates (x100) the display offers at its current resolution (android_platform.c: the rate a
     *  stream asks its host for, as the webOS build asks for its panel's). */
    static int[] getRefreshRates() {
        Display d = display();
        return d != null ? ratesX100(modesAtCurrentSize(d)) : new int[0];
    }

    /** {width, height, current refresh x100, highest refresh x100 at that size, HDR10 0/1} (android_platform.c). */
    static int[] getDisplayMode() {
        Display d = display();
        if (d == null) {
            return null;
        }
        Display.Mode mode = d.getMode();
        int width = mode.getPhysicalWidth(), height = mode.getPhysicalHeight();
        float max = mode.getRefreshRate();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() == width && m.getPhysicalHeight() == height && m.getRefreshRate() > max) {
                max = m.getRefreshRate();
            }
        }
        return new int[]{width, height, Math.round(mode.getRefreshRate() * 100f), Math.round(max * 100f),
                displayHdr10(d) ? 1 : 0};
    }

    /**
     * Switch the TV to the display mode at the current resolution whose refresh rate is nearest the stream's
     * (0 = the system's own choice again). The webOS build gets this from its panel; Moonlight for Android does
     * the same with preferredDisplayModeId. Android 11 also gets the frame rate on the video surface.
     */
    static void requestRefreshRate(final int refreshX100) {
        final EclipseActivity activity = sInstance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            Display d = display();
            if (d == null) {
                return;
            }
            WindowManager.LayoutParams params = activity.getWindow().getAttributes();
            int modeId = 0;
            if (refreshX100 > 0) {
                Display.Mode[] modes = modesAtCurrentSize(d);
                int best = DisplayModes.pick(ratesX100(modes), refreshX100);
                modeId = best >= 0 ? modes[best].getModeId() : 0;
            }
            if (params.preferredDisplayModeId != modeId) {
                params.preferredDisplayModeId = modeId;
                activity.getWindow().setAttributes(params);
                Log.i(TAG, "Display mode " + modeId + " for " + refreshX100 / 100f + " Hz");
            }
            Surface surface = sVideoSurface;
            if (Build.VERSION.SDK_INT >= 30 && surface != null && surface.isValid()) {
                try {
                    surface.setFrameRate(refreshX100 > 0 ? refreshX100 / 100f : 0f,
                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                } catch (IllegalArgumentException | IllegalStateException e) {
                    Log.w(TAG, "setFrameRate", e);
                }
            }
        });
    }

    /**
     * Show the home screen, as the Home key does. A controller's Guide button is Home on the SHIELD TV only
     * until its rewrite is turned off so that Guide reaches the host; View + Start is then the controller's way
     * home during a stream (session_gamepad.c).
     */
    static void goHome() {
        final EclipseActivity activity = sInstance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            Intent home = new Intent(Intent.ACTION_MAIN);
            home.addCategory(Intent.CATEGORY_HOME);
            home.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try {
                activity.startActivity(home);
            } catch (RuntimeException e) {
                Log.w(TAG, "goHome", e);
            }
        });
    }

    /* --- video ------------------------------------------------------------------------------------------ */

    /** The surface the MediaCodec module renders into; null until Android has created it. */
    static Surface getVideoSurface() {
        Surface surface = sVideoSurface;
        return surface != null && surface.isValid() ? surface : null;
    }

    /** Letterbox the video plane to the stream's shape (0x0 = the whole screen). */
    static void setVideoSize(final int width, final int height) {
        final EclipseActivity activity = sInstance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            if (activity.mVideoView == null || (activity.mVideoWidth == width && activity.mVideoHeight == height)) {
                return;
            }
            activity.mVideoWidth = width;
            activity.mVideoHeight = height;
            activity.mVideoView.setLayoutParams(activity.videoLayout(width, height));
        });
    }

    private RelativeLayout.LayoutParams videoLayout(int width, int height) {
        int w = ViewGroup.LayoutParams.MATCH_PARENT, h = ViewGroup.LayoutParams.MATCH_PARENT;
        int screenW = mLayout != null ? mLayout.getWidth() : 0, screenH = mLayout != null ? mLayout.getHeight() : 0;
        if (width > 0 && height > 0 && screenW > 0 && screenH > 0) {
            /* Integer cross-multiplication: the same shape is the whole screen, exactly. */
            long a = (long) width * screenH, b = (long) height * screenW;
            if (a > b) {
                w = screenW;
                h = (int) ((long) screenW * height / width);
            } else if (a < b) {
                h = screenH;
                w = (int) ((long) screenH * width / height);
            }
        }
        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(w, h);
        params.addRule(RelativeLayout.CENTER_IN_PARENT);
        return params;
    }

    /**
     * The video decoders, best first for each format, for the MediaCodec module:
     * a first line "display|hdr10" and then "mime|name|hardware|lowLatency|main10|maxWidth|maxHeight".
     * Hardware decoders win; a software one is listed only when a format has no other (the emulator).
     */
    static String probeVideoDecoders() {
        StringBuilder out = new StringBuilder();
        Display d = display();
        out.append("display|").append(d != null && displayHdr10(d) ? 1 : 0).append('\n');
        String[] mimes = {"video/hevc", "video/avc", "video/av01", "video/x-vnd.on2.vp9"};
        MediaCodecInfo[] infos = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        for (String mime : mimes) {
            MediaCodecInfo best = null;
            boolean bestHardware = false;
            for (MediaCodecInfo info : infos) {
                if (info.isEncoder() || info.getName().toLowerCase().endsWith(".secure")) {
                    continue;
                }
                boolean supports = false;
                for (String type : info.getSupportedTypes()) {
                    if (type.equalsIgnoreCase(mime)) {
                        supports = true;
                        break;
                    }
                }
                if (!supports) {
                    continue;
                }
                boolean hardware = isHardware(info);
                if (best == null || (hardware && !bestHardware)) {
                    best = info;
                    bestHardware = hardware;
                }
            }
            if (best == null) {
                continue;
            }
            MediaCodecInfo.CodecCapabilities caps = best.getCapabilitiesForType(mime);
            boolean lowLatency = Build.VERSION.SDK_INT >= 30 &&
                    caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency);
            boolean main10 = false;
            for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) {
                if (mime.equals("video/hevc") && (pl.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10 ||
                        pl.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10)) {
                    main10 = true;
                } else if (mime.equals("video/av01") && (pl.profile == MediaCodecInfo.CodecProfileLevel.AV1ProfileMain10 ||
                        pl.profile == MediaCodecInfo.CodecProfileLevel.AV1ProfileMain10HDR10)) {
                    main10 = true;
                }
            }
            int maxWidth = 1920, maxHeight = 1080;
            MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
            if (video != null) {
                maxWidth = video.getSupportedWidths().getUpper();
                maxHeight = video.getSupportedHeights().getUpper();
            }
            out.append(mime).append('|').append(best.getName()).append('|').append(bestHardware ? 1 : 0)
                    .append('|').append(lowLatency ? 1 : 0).append('|').append(main10 ? 1 : 0)
                    .append('|').append(maxWidth).append('|').append(maxHeight).append('\n');
        }
        return out.toString();
    }

    private static boolean isHardware(MediaCodecInfo info) {
        if (Build.VERSION.SDK_INT >= 29) {
            return info.isHardwareAccelerated();
        }
        String name = info.getName().toLowerCase();
        return !(name.startsWith("omx.google.") || name.startsWith("c2.android.") || name.startsWith("omx.ffmpeg."));
    }
}

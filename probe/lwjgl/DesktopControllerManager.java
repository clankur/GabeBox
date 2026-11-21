package com.badlogic.gdx.controllers.desktop;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import com.badlogic.gdx.controllers.Controller;
import com.badlogic.gdx.controllers.ControllerListener;
import com.badlogic.gdx.controllers.ControllerManager;
import com.badlogic.gdx.controllers.PovDirection;
import com.badlogic.gdx.math.Vector3;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.SnapshotArray;
import org.lwjgl.opengl.XboxSdlGamepad;

/** Connects libGDX's legacy desktop controller API to the SDL gamepad. */
public final class DesktopControllerManager implements ControllerManager {
    // SDL button index for each Windows Xbox 360 DirectInput button.
    private static final int[] BUTTONS = {0, 1, 2, 3, 9, 10, 4, 6, 7, 8};
    private static final int[] STICKS = {1, 0, 3, 2};
    private final IntBuffer sample = ByteBuffer.allocateDirect(24).order(ByteOrder.nativeOrder()).asIntBuffer();
    private final Array<Controller> controllers = new Array<>();
    private final SnapshotArray<ControllerListener> listeners = new SnapshotArray<>(true, 4, ControllerListener.class);
    private final Pad pad = new Pad();

    public DesktopControllerManager() {
        refresh();
        XboxSdlGamepad.listen(this::refresh);
    }

    @Override public Array<Controller> getControllers() { return controllers; }
    @Override public void addListener(ControllerListener listener) { listeners.add(listener); }
    @Override public void removeListener(ControllerListener listener) { listeners.removeValue(listener, true); }
    @Override public void clearListeners() { listeners.clear(); }
    @Override public Array<ControllerListener> getListeners() { return listeners; }

    private interface Event { void deliver(ControllerListener listener); }

    private static void dispatch(SnapshotArray<ControllerListener> targets, Event event) {
        int count = targets.size;
        ControllerListener[] snapshot = targets.begin();
        try {
            for (int i = 0; i < count; ++i) event.deliver(snapshot[i]);
        } finally {
            targets.end();
        }
    }

    private void fire(Event event) {
        dispatch(listeners, event);
        dispatch(pad.listeners, event);
    }

    private void refresh() {
        int mask = XboxSdlGamepad.read(sample);
        if (mask < 0) {
            if (controllers.size != 0) {
                update(0, false);
                controllers.clear();
                fire(listener -> listener.disconnected(pad));
            }
            return;
        }
        if (controllers.size == 0) {
            controllers.add(pad);
            System.out.println("slay_sdl_gamepad=connected");
            fire(listener -> listener.connected(pad));
        }
        update(mask, true);
    }

    private void update(int mask, boolean connected) {
        int previous = pad.mask;
        pad.mask = mask;
        for (int i = 0; i < BUTTONS.length; ++i) {
            final int button = i;
            if (((previous ^ mask) & (1 << BUTTONS[button])) != 0) {
                if (pad.getButton(button)) fire(listener -> listener.buttonDown(pad, button));
                else fire(listener -> listener.buttonUp(pad, button));
            }
        }
        for (int i = 0; i < pad.axes.length; ++i) {
            final int axis = i;
            float value = 0;
            if (connected) {
                value = axis == STICKS.length ? (sample.get(4) - sample.get(5)) / 32767f :
                        Math.max(-1f, sample.get(STICKS[axis]) / 32767f);
            }
            if (pad.axes[axis] != value) {
                pad.axes[axis] = value;
                fire(listener -> listener.axisMoved(pad, axis, pad.axes[axis]));
            }
        }
        PovDirection pov = pad.getPov(0);
        if (pov != direction(previous)) fire(listener -> listener.povMoved(pad, 0, pov));
    }

    private static PovDirection direction(int mask) {
        boolean up = (mask & 2048) != 0, down = (mask & 4096) != 0;
        boolean left = (mask & 8192) != 0, right = (mask & 16384) != 0;
        if (up != down) {
            if (left != right) return up ? (left ? PovDirection.northWest : PovDirection.northEast)
                    : (left ? PovDirection.southWest : PovDirection.southEast);
            return up ? PovDirection.north : PovDirection.south;
        }
        return left == right ? PovDirection.center : (left ? PovDirection.west : PovDirection.east);
    }

    private static final class Pad implements Controller {
        int mask;
        final float[] axes = new float[5];
        final Vector3 gravity = new Vector3();
        final SnapshotArray<ControllerListener> listeners = new SnapshotArray<>(true, 4, ControllerListener.class);

        @Override public boolean getButton(int code) {
            return code >= 0 && code < BUTTONS.length && (mask & (1 << BUTTONS[code])) != 0;
        }
        @Override public float getAxis(int code) { return code >= 0 && code < axes.length ? axes[code] : 0; }
        @Override public PovDirection getPov(int code) { return code == 0 ? direction(mask) : PovDirection.center; }
        @Override public boolean getSliderX(int code) { return false; }
        @Override public boolean getSliderY(int code) { return false; }
        @Override public Vector3 getAccelerometer(int code) { return gravity; }
        @Override public void setAccelerometerSensitivity(float sensitivity) {}
        @Override public String getName() { return "Controller (Xbox 360 For Windows)"; }
        @Override public void addListener(ControllerListener listener) { listeners.add(listener); }
        @Override public void removeListener(ControllerListener listener) { listeners.removeValue(listener, true); }
    }
}

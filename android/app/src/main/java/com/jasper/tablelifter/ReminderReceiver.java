package com.jasper.tablelifter;

import android.Manifest;
import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.SystemClock;

/** Phone-only sit/stand reminders; this class never accesses Bluetooth. */
public final class ReminderReceiver extends BroadcastReceiver {
    private static final String REMIND = "com.jasper.tablelifter.REMIND";
    private static final String CHANNEL = "sit_stand";

    /** Reschedule after reboot or display one reminder notification. */
    @Override public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            schedule(context);
        } else if (REMIND.equals(intent.getAction()) &&
                   context.getSharedPreferences("MainActivity", Context.MODE_PRIVATE)
                          .getBoolean("reminders", false)) {
            show(context);
        }
    }

    /** Set or cancel an inexact repeating reminder using saved preferences. */
    static void schedule(Context context) {
        AlarmManager manager = context.getSystemService(AlarmManager.class);
        PendingIntent pending = PendingIntent.getBroadcast(context, 1,
                new Intent(context, ReminderReceiver.class).setAction(REMIND),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        manager.cancel(pending);
        boolean enabled = context.getSharedPreferences("MainActivity", Context.MODE_PRIVATE)
                                 .getBoolean("reminders", false);
        if (!enabled) return;
        int index = context.getSharedPreferences("MainActivity", Context.MODE_PRIVATE)
                           .getInt("interval", 1);
        long[] minutes = {30, 60, 90};
        long interval = minutes[Math.max(0, Math.min(index, 2))] * 60_000L;
        manager.setInexactRepeating(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                SystemClock.elapsedRealtime() + interval, interval, pending);
    }

    /** Display a notification that requires a human decision to move. */
    private static void show(Context context) {
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
                    PackageManager.PERMISSION_GRANTED) return;
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                "Sit and stand reminders", NotificationManager.IMPORTANCE_DEFAULT));
        PendingIntent open = PendingIntent.getActivity(context, 2,
                new Intent(context, MainActivity.class),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification notification = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Time to change position")
                .setContentText("Sit or stand when it suits you. Open the table controls to move.")
                .setContentIntent(open).setAutoCancel(true).build();
        manager.notify(1, notification);
    }
}

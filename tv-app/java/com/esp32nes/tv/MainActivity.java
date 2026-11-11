package com.esp32nes.tv;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicReference;

public class MainActivity extends Activity {
    private static final String PREFS = "esp32nes";
    private static final int DISCOVERY_PORT = 8266;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final List<Rom> roms = new ArrayList<>();
    private ArrayAdapter<Rom> adapter;
    private ListView list;
    private TextView status;
    private String ip;
    private SharedPreferences prefs;
    private volatile boolean busy;

    static class Rom {
        String name;
        long size;
        boolean builtin;

        @Override
        public String toString() {
            return name;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        ip = prefs.getString("ip", null);
        setContentView(buildUi());
        startDiscover();
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(36), dp(24), dp(36), dp(20));
        root.setBackgroundColor(Color.rgb(11, 14, 22));

        TextView title = new TextView(this);
        title.setText("ESP32-NES");
        title.setTextSize(42);
        title.setTextColor(Color.rgb(90, 209, 255));
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("TV 模式 · 手柄保持连接 ESP32 · 遥控器选择游戏");
        sub.setTextSize(16);
        sub.setTextColor(Color.rgb(139, 151, 173));
        sub.setPadding(0, dp(4), 0, dp(16));
        root.addView(sub);

        status = new TextView(this);
        status.setText("准备中…");
        status.setTextSize(18);
        status.setTextColor(Color.rgb(255, 209, 90));
        status.setPadding(0, 0, 0, dp(12));
        root.addView(status);

        list = new ListView(this);
        list.setDivider(null);
        list.setSelector(new ColorDrawable(Color.rgb(23, 50, 74)));
        list.setDrawSelectorOnTop(false);
        list.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        list.setItemsCanFocus(false);
        list.setFocusable(true);
        list.setFocusableInTouchMode(true);
        list.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        adapter = new ArrayAdapter<Rom>(this, android.R.layout.simple_list_item_1, roms) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getView(position, convertView, parent);
                tv.setTextSize(24);
                tv.setTextColor(roms.get(position).builtin
                        ? Color.rgb(90, 209, 255) : Color.rgb(232, 237, 247));
                tv.setPadding(dp(20), dp(14), dp(20), dp(14));
                return tv;
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                startGame(roms.get(position));
            }
        });
        root.addView(list);

        LinearLayout footer = new LinearLayout(this);
        footer.setOrientation(LinearLayout.HORIZONTAL);
        footer.setPadding(0, dp(12), 0, 0);

        TextView hint = new TextView(this);
        hint.setText("方向键选择 · OK 开始 · 返回退出");
        hint.setTextSize(14);
        hint.setTextColor(Color.rgb(95, 106, 128));
        hint.setLayoutParams(new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        footer.addView(hint);

        Button manual = new Button(this);
        manual.setText("手动输入 IP");
        manual.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showManualIp();
            }
        });
        footer.addView(manual);
        root.addView(footer);
        return root;
    }

    private void setStatus(final String text) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                status.setText(text);
            }
        });
    }

    private void startDiscover() {
        if (busy) {
            return;
        }
        busy = true;
        setStatus("正在搜索 ESP32…");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String found = discover();
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        if (found == null) {
                            setStatus("未发现设备：请确认同一局域网，或点右下角手动输入 IP");
                            return;
                        }
                        ip = found;
                        prefs.edit().putString("ip", ip).apply();
                        setStatus("已连接 " + ip + "，读取 ROM…");
                        loadRoms();
                    }
                });
            }
        }).start();
    }

    private String discover() {
        String found = discoverUdp();
        if (found != null) {
            return found;
        }
        if (ip != null && isDevice(ip)) {
            return ip;
        }
        setStatus("广播未发现，正在扫描局域网…");
        found = scanSubnet();
        if (found != null) {
            return found;
        }
        return null;
    }

    private String discoverUdp() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        WifiManager.MulticastLock lock =
                wm != null ? wm.createMulticastLock("esp32nes") : null;
        DatagramSocket socket = null;
        try {
            if (lock != null) {
                lock.acquire();
            }
            socket = new DatagramSocket();
            socket.setBroadcast(true);
            socket.setSoTimeout(300);
            byte[] msg = "ESP32NES_DISCOVER".getBytes("UTF-8");
            Set<String> targets = new HashSet<String>();
            targets.add("255.255.255.255");
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) {
                    continue;
                }
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null) {
                        targets.add(b.getHostAddress());
                    }
                }
            }
            for (String target : targets) {
                for (int i = 0; i < 2; i++) {
                    DatagramPacket out = new DatagramPacket(msg, msg.length,
                            InetAddress.getByName(target), DISCOVERY_PORT);
                    socket.send(out);
                }
            }
            byte[] buf = new byte[128];
            long deadline = System.currentTimeMillis() + 2000;
            while (System.currentTimeMillis() < deadline) {
                try {
                    DatagramPacket in = new DatagramPacket(buf, buf.length);
                    socket.receive(in);
                    String text = new String(in.getData(), 0, in.getLength(), "UTF-8").trim();
                    if (text.startsWith("ESP32NES ")) {
                        String host = text.substring("ESP32NES ".length()).trim();
                        if (isDevice(host)) {
                            return host;
                        }
                    }
                } catch (SocketTimeoutException ignored) {
                }
            }
        } catch (Exception e) {
            // fall through to subnet scan
        } finally {
            if (socket != null) {
                socket.close();
            }
            if (lock != null) {
                lock.release();
            }
        }
        return null;
    }

    private String scanSubnet() {
        final String local = getLocalIpv4();
        if (local == null) {
            return null;
        }
        int dot = local.lastIndexOf('.');
        if (dot <= 0) {
            return null;
        }
        final String prefix = local.substring(0, dot + 1);
        final AtomicReference<String> found = new AtomicReference<String>();
        ExecutorService pool = Executors.newFixedThreadPool(16);
        List<Future<?>> futures = new ArrayList<Future<?>>();
        for (int i = 1; i <= 254; i++) {
            final String candidate = prefix + i;
            if (candidate.equals(local)) {
                continue;
            }
            futures.add(pool.submit(new Runnable() {
                @Override
                public void run() {
                    if (found.get() != null) {
                        return;
                    }
                    if (isDevice(candidate)) {
                        found.compareAndSet(null, candidate);
                    }
                }
            }));
        }
        long deadline = System.currentTimeMillis() + 7000;
        while (System.currentTimeMillis() < deadline && found.get() == null) {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                break;
            }
        }
        pool.shutdownNow();
        return found.get();
    }

    private String getLocalIpv4() {
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) {
                    continue;
                }
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress addr = ia.getAddress();
                    if (addr instanceof Inet4Address && !addr.isLoopbackAddress()
                            && !addr.isLinkLocalAddress()) {
                        return addr.getHostAddress();
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private boolean isDevice(String host) {
        try {
            Socket probe = new Socket();
            probe.connect(new InetSocketAddress(host, 80), 150);
            probe.close();
        } catch (Exception e) {
            return false;
        }
        String body = httpGet(host, "/api/status", 800);
        return body != null && body.contains("\"name\":\"ESP32-NES\"");
    }

    private String httpGet(String host, String path, int timeoutMs) {
        HttpURLConnection conn = null;
        try {
            URL url = new URL("http://" + host + path);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(timeoutMs);
            conn.setReadTimeout(timeoutMs);
            conn.setRequestProperty("Cache-Control", "no-store");
            if (conn.getResponseCode() != 200) {
                return null;
            }
            InputStream in = conn.getInputStream();
            BufferedReader reader = new BufferedReader(new InputStreamReader(in, "UTF-8"));
            StringBuilder sb = new StringBuilder();
            char[] buf = new char[2048];
            int n;
            while ((n = reader.read(buf)) > 0) {
                sb.append(buf, 0, n);
            }
            reader.close();
            return sb.toString();
        } catch (Exception e) {
            return null;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private void loadRoms() {
        setStatus("读取 ROM 列表…");
        new Thread(new Runnable() {
            @Override
            public void run() {
                String body = ip != null ? httpGet(ip, "/api/roms", 3000) : null;
                final List<Rom> parsed = new ArrayList<Rom>();
                if (body != null) {
                    try {
                        JSONArray arr = new JSONObject(body).optJSONArray("roms");
                        if (arr != null) {
                            for (int i = 0; i < arr.length(); i++) {
                                JSONObject o = arr.getJSONObject(i);
                                Rom r = new Rom();
                                r.name = o.optString("name");
                                r.size = o.optLong("size");
                                r.builtin = o.optBoolean("builtin");
                                if (r.name.length() > 0) {
                                    parsed.add(r);
                                }
                            }
                        }
                    } catch (Exception ignored) {
                    }
                }
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        roms.clear();
                        roms.addAll(parsed);
                        adapter.notifyDataSetChanged();
                        if (roms.isEmpty()) {
                            setStatus("设备上没有 ROM，请先在网页上传");
                        } else {
                            list.setItemChecked(0, true);
                            list.setSelection(0);
                            list.requestFocus();
                            setStatus("已连接 " + ip + " · 选择游戏后按 OK");
                        }
                    }
                });
            }
        }).start();
    }

    private void startGame(Rom rom) {
        if (ip == null) {
            Toast.makeText(this, "设备未连接", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("ip", ip);
        intent.putExtra("rom", rom.name);
        startActivity(intent);
    }

    private void showManualIp() {
        final EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setText(ip != null ? ip : "192.168.1.");
        new AlertDialog.Builder(this)
                .setTitle("ESP32 的 IP")
                .setView(input)
                .setPositiveButton("连接", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        String value = input.getText().toString().trim();
                        if (value.length() == 0) {
                            return;
                        }
                        ip = value;
                        prefs.edit().putString("ip", ip).apply();
                        busy = true;
                        new Thread(new Runnable() {
                            @Override
                            public void run() {
                                final boolean ok = isDevice(ip);
                                ui.post(new Runnable() {
                                    @Override
                                    public void run() {
                                        busy = false;
                                        if (ok) {
                                            setStatus("已连接 " + ip + "，读取 ROM…");
                                            loadRoms();
                                        } else {
                                            setStatus("连接失败：请检查 IP 和局域网");
                                        }
                                    }
                                });
                            }
                        }).start();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            immersive();
        }
    }
}

package net.seilent.seifg

import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ResolveInfo
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import kotlin.math.roundToInt

data class AppEntry(
    val label: String,
    val packageName: String,
    val icon: Drawable
)

data class AppConfig(
    var enabled: Boolean = false,
    var targetFps: Int = 60,
    var multiplier: Int = 2,
    var quality: Int = 2
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val colorScheme = if (Build.VERSION.SDK_INT >= 31) {
                dynamicDarkColorScheme(this)
            } else {
                darkColorScheme()
            }
            MaterialTheme(colorScheme = colorScheme) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    ConfigScreen()
                }
            }
        }
    }
}

@Suppress("DEPRECATION")
fun getDisplayRefreshRate(activity: ComponentActivity): Int {
    val rate = if (Build.VERSION.SDK_INT >= 30) {
        activity.display?.refreshRate ?: 60f
    } else {
        val wm = activity.getSystemService(android.content.Context.WINDOW_SERVICE) as android.view.WindowManager
        wm.defaultDisplay.refreshRate
    }
    return rate.toInt()
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val refreshHz = remember { getDisplayRefreshRate(context as ComponentActivity) }

    var hasRoot by remember { mutableStateOf<Boolean?>(null) }
    var apps by remember { mutableStateOf<List<AppEntry>>(emptyList()) }
    var configs by remember { mutableStateOf(mutableMapOf<String, AppConfig>()) }
    var searchQuery by remember { mutableStateOf("") }
    var saving by remember { mutableStateOf(false) }
    var snackMessage by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            Shell.getShell()
            hasRoot = Shell.isAppGrantedRoot() == true

            if (hasRoot == true) {
                val result = Shell.cmd("cat /data/local/tmp/TargetList.json").exec()
                if (result.isSuccess && result.out.isNotEmpty()) {
                    try {
                        val json = JSONObject(result.out.joinToString(""))
                        val custom = json.optJSONObject("custom")
                        if (custom != null) {
                            val map = mutableMapOf<String, AppConfig>()
                            for (key in custom.keys()) {
                                val obj = custom.getJSONObject(key)
                                val mult = obj.optInt("multiplier", 2)
                                val fps = obj.optInt("fps", 30)
                                val targetFps = obj.optInt("target_fps", fps * mult)
                                val quality = obj.optInt("quality", 2)
                                map[key] = AppConfig(
                                    enabled = true,
                                    targetFps = targetFps.coerceIn(30, refreshHz),
                                    multiplier = mult.coerceIn(2, 3),
                                    quality = quality.coerceIn(0, 2)
                                )
                            }
                            configs = map
                        }
                    } catch (_: Exception) {}
                }
            }

            val pm = context.packageManager
            val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            val resolved: List<ResolveInfo> = pm.queryIntentActivities(intent, PackageManager.MATCH_ALL)
            apps = resolved
                .filter { it.activityInfo.packageName != context.packageName }
                .map { ri ->
                    AppEntry(
                        label = ri.loadLabel(pm).toString(),
                        packageName = ri.activityInfo.packageName,
                        icon = ri.loadIcon(pm)
                    )
                }
                .distinctBy { it.packageName }
                .sortedWith(compareByDescending<AppEntry> { configs.containsKey(it.packageName) }.thenBy { it.label.lowercase() })
        }
    }

    val filtered = remember(apps, searchQuery) {
        if (searchQuery.isBlank()) apps
        else apps.filter {
            it.label.contains(searchQuery, ignoreCase = true) ||
                    it.packageName.contains(searchQuery, ignoreCase = true)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("seifg") },
                actions = {
                    TextButton(
                        onClick = {
                            saving = true
                            scope.launch(Dispatchers.IO) {
                                val json = JSONObject()
                                val custom = JSONObject()
                                for ((pkg, cfg) in configs) {
                                    if (cfg.enabled) {
                                        val cap = maxOf(1, (cfg.targetFps.toFloat() / cfg.multiplier).roundToInt())
                                        val entry = JSONObject()
                                        entry.put("fps", cap)
                                        entry.put("multiplier", cfg.multiplier)
                                        entry.put("quality", cfg.quality)
                                        entry.put("target_fps", cfg.targetFps)
                                        custom.put(pkg, entry)
                                    }
                                }
                                json.put("custom", custom)

                                val cacheFile = File(context.cacheDir, "TargetList.json")
                                cacheFile.writeText(json.toString(2))

                                val result = Shell.cmd(
                                    "cp ${cacheFile.absolutePath} /data/local/tmp/TargetList.json && chmod 644 /data/local/tmp/TargetList.json"
                                ).exec()

                                withContext(Dispatchers.Main) {
                                    saving = false
                                    snackMessage = if (result.isSuccess) "Saved" else "Write failed"
                                }
                            }
                        },
                        enabled = hasRoot == true && !saving
                    ) {
                        Text("Save")
                    }
                }
            )
        },
        snackbarHost = {
            snackMessage?.let { msg ->
                Snackbar(
                    action = {
                        TextButton(onClick = { snackMessage = null }) { Text("OK") }
                    }
                ) { Text(msg) }
            }
        }
    ) { padding ->
        Column(modifier = Modifier.padding(padding)) {
            if (hasRoot == false) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                    modifier = Modifier.fillMaxWidth().padding(16.dp)
                ) {
                    Text(
                        "Root access unavailable. Cannot read or write config.",
                        modifier = Modifier.padding(16.dp),
                        color = MaterialTheme.colorScheme.onErrorContainer
                    )
                }
            }

            OutlinedTextField(
                value = searchQuery,
                onValueChange = { searchQuery = it },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                placeholder = { Text("Search apps") },
                singleLine = true
            )

            if (apps.isEmpty() && hasRoot != null) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
            } else {
                LazyColumn(modifier = Modifier.fillMaxSize()) {
                    items(filtered, key = { it.packageName }) { app ->
                        val cfg = configs.getOrPut(app.packageName) { AppConfig(targetFps = minOf(60, refreshHz)) }
                        AppRow(app, cfg, refreshHz) { updated ->
                            configs = configs.toMutableMap().also { it[app.packageName] = updated }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun AppRow(app: AppEntry, config: AppConfig, refreshHz: Int, onUpdate: (AppConfig) -> Unit) {
    val multiplierOptions = listOf(2, 3)
    val qualityLabels = listOf("Performance", "Balanced", "Quality")
    val can3x = refreshHz > 60

    Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Image(
                bitmap = app.icon.toBitmap(48, 48).asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.size(40.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(app.label, style = MaterialTheme.typography.bodyLarge)
                Text(app.packageName, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onUpdate(config.copy(enabled = it)) }
            )
        }

        AnimatedVisibility(visible = config.enabled) {
            Column(modifier = Modifier.padding(start = 52.dp, top = 4.dp, bottom = 8.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Text("Multiplier", style = MaterialTheme.typography.labelMedium)
                    SingleChoiceSegmentedButtonRow {
                        multiplierOptions.forEachIndexed { index, mult ->
                            SegmentedButton(
                                selected = config.multiplier == mult,
                                onClick = {
                                    if (mult == 3 && !can3x) return@SegmentedButton
                                    onUpdate(config.copy(multiplier = mult))
                                },
                                shape = SegmentedButtonDefaults.itemShape(index, multiplierOptions.size),
                                enabled = mult != 3 || can3x
                            ) {
                                Text("${mult}x")
                            }
                        }
                    }
                }
                if (!can3x) {
                    Text(
                        "3x needs a >60 Hz display",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                Spacer(Modifier.height(8.dp))

                Text("Target FPS: ${config.targetFps}", style = MaterialTheme.typography.labelMedium)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TextButton(
                        onClick = {
                            val newVal = (config.targetFps - 5).coerceIn(30, refreshHz)
                            onUpdate(config.copy(targetFps = newVal))
                        },
                        contentPadding = PaddingValues(0.dp),
                        modifier = Modifier.size(36.dp)
                    ) { Text("-") }
                    Slider(
                        value = config.targetFps.toFloat(),
                        onValueChange = { onUpdate(config.copy(targetFps = (it / 5).roundToInt() * 5)) },
                        valueRange = 30f..refreshHz.toFloat(),
                        steps = ((refreshHz - 30) / 5) - 1,
                        modifier = Modifier.weight(1f)
                    )
                    TextButton(
                        onClick = {
                            val newVal = (config.targetFps + 5).coerceIn(30, refreshHz)
                            onUpdate(config.copy(targetFps = newVal))
                        },
                        contentPadding = PaddingValues(0.dp),
                        modifier = Modifier.size(36.dp)
                    ) { Text("+") }
                }

                Spacer(Modifier.height(8.dp))

                Text("Quality", style = MaterialTheme.typography.labelMedium)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    qualityLabels.forEachIndexed { index, label ->
                        SegmentedButton(
                            selected = config.quality == index,
                            onClick = { onUpdate(config.copy(quality = index)) },
                            shape = SegmentedButtonDefaults.itemShape(index, qualityLabels.size)
                        ) {
                            Text(label, style = MaterialTheme.typography.labelSmall)
                        }
                    }
                }

                Spacer(Modifier.height(8.dp))

                val cap = maxOf(1, (config.targetFps.toFloat() / config.multiplier).roundToInt())
                Text(
                    "Output ${config.targetFps} FPS  |  render cap $cap FPS",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        HorizontalDivider()
    }
}

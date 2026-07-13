package com.seilent.sefg

import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ResolveInfo
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusGroup
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.key.*
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.graphics.drawable.toBitmap
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import kotlin.math.abs

object Tok {
    val accent = Color(0xFF5EC6B2)
    val accentDim = Color(0xFF3D9E8D)
    val accentSurface = Color(0xFF1A3B36)
    val accentOnSurface = Color(0xFFB8E8DD)

    val bg = Color(0xFF101314)
    val surface = Color(0xFF181C1E)
    val surfaceRaised = Color(0xFF1F2426)
    val surfaceBright = Color(0xFF272D30)

    val textPrimary = Color(0xFFE4E7E8)
    val textSecondary = Color(0xFF8D9599)
    val textMuted = Color(0xFF5C6569)

    val border = Color(0xFF2A3034)
    val borderFocus = accent

    val radius = 10.dp
    val radiusSmall = 8.dp

    val sp4 = 4.dp
    val sp6 = 6.dp
    val sp8 = 8.dp
    val sp12 = 12.dp
    val sp16 = 16.dp
    val sp20 = 20.dp
    val sp24 = 24.dp

    val shape = RoundedCornerShape(radius)
    val shapeSmall = RoundedCornerShape(radiusSmall)
}

private val SefgColorScheme = darkColorScheme(
    primary = Tok.accent,
    onPrimary = Color(0xFF0A1F1B),
    primaryContainer = Tok.accentSurface,
    onPrimaryContainer = Tok.accentOnSurface,
    surface = Tok.surface,
    onSurface = Tok.textPrimary,
    surfaceVariant = Tok.surfaceRaised,
    onSurfaceVariant = Tok.textSecondary,
    background = Tok.bg,
    onBackground = Tok.textPrimary,
    outline = Tok.border,
    outlineVariant = Tok.border
)

data class AppEntry(
    val label: String,
    val packageName: String,
    val icon: Drawable
)

data class AppConfig(
    var enabled: Boolean = false,
    var targetFps: Int = 60,
    var multiplier: Int = 2,
    var quality: Int = 0,
    var renderScale: Float = 1.0f
)

fun validOutputs(refreshHz: Int, multiplier: Int, minBase: Int = 30): List<Int> {
    val results = mutableListOf<Int>()
    var k = 1
    while (refreshHz / k >= minBase) {
        val output = refreshHz / k
        if (refreshHz % k == 0 && output % multiplier == 0 && output / multiplier >= minBase) {
            results.add(output)
        }
        k++
    }
    return results
}

fun snapToNearest(current: Int, valid: List<Int>): Int {
    return valid.minByOrNull { abs(it - current) } ?: valid.firstOrNull() ?: 60
}

suspend fun writeConfig(configs: Map<String, AppConfig>, cacheDir: File) {
    withContext(Dispatchers.IO) {
        val json = JSONObject()
        val custom = JSONObject()
        for ((pkg, cfg) in configs) {
            if (cfg.enabled) {
                val entry = JSONObject()
                entry.put("fps", cfg.targetFps / cfg.multiplier)
                entry.put("multiplier", cfg.multiplier)
                entry.put("quality", cfg.quality)
                entry.put("target_fps", cfg.targetFps)
                entry.put("render_scale", cfg.renderScale)
                custom.put(pkg, entry)
            }
        }
        json.put("custom", custom)

        val cacheFile = File(cacheDir, "TargetList.json")
        cacheFile.writeText(json.toString(2))

        Shell.cmd(
            "cp ${cacheFile.absolutePath} /data/local/tmp/TargetList.json && chmod 644 /data/local/tmp/TargetList.json"
        ).exec()

        val scaleCmds = configs.entries.joinToString(" ; ") { (pkg, cfg) ->
            val path = "/sdcard/Android/data/$pkg/files/seifg_render_scale"
            if (cfg.enabled && cfg.renderScale < 0.999f) {
                "mkdir -p /sdcard/Android/data/$pkg/files && echo ${String.format("%.3f", cfg.renderScale)} > $path && chmod 644 $path"
            } else {
                "rm -f $path"
            }
        }
        Shell.cmd(scaleCmds).exec()
    }
}

@Suppress("DEPRECATION")
fun getDisplayRefreshRate(activity: ComponentActivity): Int {
    val display = if (Build.VERSION.SDK_INT >= 30) {
        activity.display
    } else {
        val wm = activity.getSystemService(android.content.Context.WINDOW_SERVICE) as android.view.WindowManager
        wm.defaultDisplay
    }
    if (display != null) {
        val maxRate = display.supportedModes.maxOfOrNull { it.refreshRate }
        if (maxRate != null) return maxRate.toInt()
        return display.refreshRate.toInt()
    }
    return 60
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = SefgColorScheme) {
                Surface(modifier = Modifier.fillMaxSize(), color = Tok.bg) {
                    ConfigScreen()
                }
            }
        }
    }
}

@Composable
fun FocusHighlight(focused: Boolean, modifier: Modifier = Modifier): Modifier {
    return if (focused) {
        modifier
            .border(2.dp, Tok.borderFocus, Tok.shape)
            .background(Tok.accent.copy(alpha = 0.06f), Tok.shape)
    } else modifier
}

@Composable
fun ControlChip(
    label: String,
    selected: Boolean,
    focused: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val interactionSource = remember { MutableInteractionSource() }
    val chipFocused by interactionSource.collectIsFocusedAsState()
    val isFocused = focused || chipFocused

    Box(
        modifier = modifier
            .height(34.dp)
            .clip(Tok.shapeSmall)
            .then(
                if (isFocused) Modifier.border(2.dp, Tok.borderFocus, Tok.shapeSmall)
                else if (selected) Modifier.border(1.dp, Tok.accentDim.copy(alpha = 0.5f), Tok.shapeSmall)
                else Modifier.border(1.dp, Tok.border, Tok.shapeSmall)
            )
            .background(
                when {
                    selected -> Tok.accent
                    isFocused -> Tok.surfaceBright
                    else -> Color.Transparent
                },
                Tok.shapeSmall
            )
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            label,
            fontSize = 13.sp,
            fontWeight = if (selected) FontWeight.Medium else FontWeight.Normal,
            color = if (selected) Tok.bg else Tok.textSecondary
        )
    }
}

@Composable
fun ControlGroup(
    title: String,
    modifier: Modifier = Modifier,
    content: @Composable RowScope.() -> Unit
) {
    Column(modifier = modifier) {
        Text(
            title,
            fontSize = 11.sp,
            color = Tok.textMuted,
            fontWeight = FontWeight.Medium,
            letterSpacing = 0.5.sp
        )
        Spacer(Modifier.height(Tok.sp6))
        Row(
            horizontalArrangement = Arrangement.spacedBy(Tok.sp6),
            content = content
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val refreshHz = remember { getDisplayRefreshRate(context as ComponentActivity) }
    val keyboardController = LocalSoftwareKeyboardController.current

    var hasRoot by remember { mutableStateOf<Boolean?>(null) }
    var apps by remember { mutableStateOf<List<AppEntry>>(emptyList()) }
    var configs by remember { mutableStateOf(mutableMapOf<String, AppConfig>()) }
    var searchQuery by remember { mutableStateOf("") }
    var advanced by remember { mutableStateOf(false) }
    var initialLoadDone by remember { mutableStateOf(false) }
    var searchActive by remember { mutableStateOf(false) }

    val listState = rememberLazyListState()
    val firstItemFocusRequester = remember { FocusRequester() }
    val searchBoxFocusRequester = remember { FocusRequester() }
    val searchFieldFocusRequester = remember { FocusRequester() }
    val gearFocusRequester = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current

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
                                val mult = obj.optInt("multiplier", 2).coerceIn(1, 3)
                                val validMults = listOf(1, 2, 3).filter { validOutputs(refreshHz, it).isNotEmpty() }
                                val effMult = if (mult in validMults) mult else (validMults.maxOrNull() ?: 1)
                                val fps = obj.optInt("fps", 30)
                                val targetFps = obj.optInt("target_fps", fps * effMult)
                                val quality = obj.optInt("quality", 0).coerceIn(0, 2)
                                val rs = obj.optDouble("render_scale", 1.0).toFloat().coerceIn(0.1f, 1.0f)
                                val valid = validOutputs(refreshHz, effMult)
                                map[key] = AppConfig(
                                    enabled = true,
                                    targetFps = snapToNearest(targetFps, valid),
                                    multiplier = effMult,
                                    quality = quality,
                                    renderScale = rs
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

            initialLoadDone = true
        }
    }

    LaunchedEffect(configs, initialLoadDone) {
        if (!initialLoadDone || hasRoot != true) return@LaunchedEffect
        delay(500)
        writeConfig(configs, context.cacheDir)
    }

    val filtered = remember(apps, searchQuery) {
        if (searchQuery.isBlank()) apps
        else apps.filter {
            it.label.contains(searchQuery, ignoreCase = true) ||
                    it.packageName.contains(searchQuery, ignoreCase = true)
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = Tok.sp20, vertical = Tok.sp12),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "SeFG",
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                color = Tok.accent,
                letterSpacing = 1.sp
            )

            Spacer(Modifier.width(Tok.sp16))

            if (searchActive) {
                OutlinedTextField(
                    value = searchQuery,
                    onValueChange = { searchQuery = it },
                    modifier = Modifier
                        .weight(1f)
                        .height(44.dp)
                        .focusRequester(searchFieldFocusRequester)
                        .onPreviewKeyEvent { event ->
                            if (event.type == KeyEventType.KeyDown && (event.key == Key.Escape || event.key == Key.Back)) {
                                searchActive = false
                                searchQuery = ""
                                keyboardController?.hide()
                                searchBoxFocusRequester.requestFocus()
                                true
                            } else false
                        },
                    placeholder = { Text("Search", fontSize = 13.sp, color = Tok.textMuted) },
                    leadingIcon = { Icon(Icons.Default.Search, contentDescription = null, modifier = Modifier.size(16.dp), tint = Tok.textMuted) },
                    singleLine = true,
                    shape = Tok.shape,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = Tok.accent,
                        unfocusedBorderColor = Tok.border,
                        cursorColor = Tok.accent
                    ),
                    textStyle = MaterialTheme.typography.bodySmall.copy(color = Tok.textPrimary)
                )

                LaunchedEffect(Unit) {
                    searchFieldFocusRequester.requestFocus()
                    keyboardController?.show()
                }
            } else {
                val searchInteraction = remember { MutableInteractionSource() }
                val searchFocused by searchInteraction.collectIsFocusedAsState()

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(44.dp)
                        .clip(Tok.shape)
                        .then(FocusHighlight(searchFocused))
                        .border(
                            1.dp,
                            if (searchFocused) Tok.borderFocus else Tok.border,
                            Tok.shape
                        )
                        .background(if (searchFocused) Tok.surfaceBright else Tok.surface, Tok.shape)
                        .clickable(
                            interactionSource = searchInteraction,
                            indication = null
                        ) { searchActive = true }
                        .focusRequester(searchBoxFocusRequester)
                        .padding(horizontal = Tok.sp12),
                    contentAlignment = Alignment.CenterStart
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.Search, contentDescription = null, modifier = Modifier.size(16.dp), tint = Tok.textMuted)
                        Spacer(Modifier.width(Tok.sp8))
                        Text(
                            if (searchQuery.isNotBlank()) searchQuery else "Search",
                            fontSize = 13.sp,
                            color = if (searchQuery.isNotBlank()) Tok.textPrimary else Tok.textMuted
                        )
                    }
                }
            }

            Spacer(Modifier.width(Tok.sp8))

            val gearInteraction = remember { MutableInteractionSource() }
            val gearFocused by gearInteraction.collectIsFocusedAsState()

            Box(
                modifier = Modifier
                    .size(44.dp)
                    .clip(Tok.shape)
                    .then(
                        if (gearFocused) Modifier.border(2.dp, Tok.borderFocus, Tok.shape)
                        else Modifier.border(1.dp, if (advanced) Tok.accentDim else Tok.border, Tok.shape)
                    )
                    .background(
                        if (advanced) Tok.accentSurface else Tok.surface,
                        Tok.shape
                    )
                    .clickable(
                        interactionSource = gearInteraction,
                        indication = null
                    ) { advanced = !advanced }
                    .focusRequester(gearFocusRequester),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    Icons.Default.Settings,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp),
                    tint = if (advanced) Tok.accent else Tok.textSecondary
                )
            }
        }

        if (hasRoot == false) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = Tok.sp20, vertical = Tok.sp8)
                    .background(Color(0xFF2D1A1A), Tok.shape)
                    .border(1.dp, Color(0xFF5C2626), Tok.shape)
                    .padding(Tok.sp12)
            ) {
                Text("No root access", fontSize = 13.sp, color = Color(0xFFE88888))
            }
        }

        if (apps.isEmpty() && hasRoot != null) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = Tok.accent, strokeWidth = 2.dp, modifier = Modifier.size(28.dp))
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(horizontal = Tok.sp16, vertical = Tok.sp8)
            ) {
                items(filtered, key = { it.packageName }) { app ->
                    val minBase = if (advanced) 10 else 30
                    val validMults = listOf(1, 2, 3).filter { validOutputs(refreshHz, it, minBase).isNotEmpty() }
                    val defaultMult = if (2 in validMults) 2 else (validMults.maxOrNull() ?: 1)
                    val defaultOutput = validOutputs(refreshHz, defaultMult, minBase).let { v ->
                        v.firstOrNull { it <= 60 } ?: v.firstOrNull() ?: 60
                    }
                    val cfg = configs.getOrPut(app.packageName) { AppConfig(targetFps = defaultOutput, multiplier = defaultMult) }
                    val isFirst = filtered.firstOrNull()?.packageName == app.packageName
                    AppCard(
                        app = app,
                        config = cfg,
                        refreshHz = refreshHz,
                        advanced = advanced,
                        focusRequester = if (isFirst) firstItemFocusRequester else null,
                        onUpdate = { updated ->
                            configs = configs.toMutableMap().also { it[app.packageName] = updated }
                        }
                    )
                    Spacer(Modifier.height(Tok.sp6))
                }
            }

            LaunchedEffect(initialLoadDone) {
                if (initialLoadDone && filtered.isNotEmpty()) {
                    try { firstItemFocusRequester.requestFocus() } catch (_: Exception) {}
                }
            }
        }
    }
}

@Composable
fun AppCard(
    app: AppEntry,
    config: AppConfig,
    refreshHz: Int,
    advanced: Boolean,
    focusRequester: FocusRequester?,
    onUpdate: (AppConfig) -> Unit
) {
    val minBase = if (advanced) 10 else 30
    val multiplierOptions = if (advanced) listOf(1, 2, 3) else listOf(1, 2, 3).filter { validOutputs(refreshHz, it, minBase).isNotEmpty() }
    val qualityLabels = listOf("Perf", "Bal", "High")

    var cardFocused by remember { mutableStateOf(false) }

    val cardMod = Modifier
        .fillMaxWidth()
        .clip(Tok.shape)
        .onFocusChanged { cardFocused = it.hasFocus }
        .then(
            if (cardFocused) Modifier.border(2.dp, Tok.borderFocus, Tok.shape)
            else if (config.enabled) Modifier.border(1.dp, Tok.accentDim.copy(alpha = 0.3f), Tok.shape)
            else Modifier.border(1.dp, Tok.border.copy(alpha = 0.5f), Tok.shape)
        )
        .background(if (config.enabled) Tok.surfaceRaised else Tok.surface, Tok.shape)
        .let { mod -> if (focusRequester != null) mod.focusRequester(focusRequester) else mod }
        .focusGroup()
        .padding(Tok.sp12)

    Column(modifier = cardMod) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            Image(
                bitmap = app.icon.toBitmap(80, 80).asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.size(32.dp)
            )
            Spacer(Modifier.width(Tok.sp12))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    app.label,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium,
                    color = Tok.textPrimary
                )
                Text(
                    app.packageName,
                    fontSize = 11.sp,
                    color = Tok.textMuted
                )
            }
            if (config.enabled) {
                Text(
                    "${config.targetFps} fps out \u00B7 cap ${config.targetFps / config.multiplier}",
                    fontSize = 11.sp,
                    color = Tok.textSecondary,
                    modifier = Modifier.padding(end = Tok.sp16)
                )
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onUpdate(config.copy(enabled = it)) },
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Tok.bg,
                    checkedTrackColor = Tok.accent,
                    uncheckedThumbColor = Tok.textMuted,
                    uncheckedTrackColor = Color.Transparent,
                    uncheckedBorderColor = Tok.border
                )
            )
        }

        AnimatedVisibility(
            visible = config.enabled,
            enter = expandVertically(),
            exit = shrinkVertically()
        ) {
            Column(modifier = Modifier.padding(top = Tok.sp12)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(Tok.sp24)
                ) {
                    ControlGroup("MULTIPLIER") {
                        multiplierOptions.forEach { mult ->
                            ControlChip(
                                label = "${mult}x",
                                selected = config.multiplier == mult,
                                focused = false,
                                onClick = {
                                    val newValid = validOutputs(refreshHz, mult, minBase)
                                    val snapped = snapToNearest(config.targetFps, newValid)
                                    onUpdate(config.copy(multiplier = mult, targetFps = snapped))
                                }
                            )
                        }
                    }

                    val outputs = validOutputs(refreshHz, config.multiplier, minBase).sorted()
                    ControlGroup("OUTPUT") {
                        outputs.forEach { fps ->
                            ControlChip(
                                label = "$fps",
                                selected = config.targetFps == fps,
                                focused = false,
                                onClick = { onUpdate(config.copy(targetFps = fps)) }
                            )
                        }
                    }

                    if (config.multiplier > 1) {
                        ControlGroup("QUALITY") {
                            qualityLabels.forEachIndexed { index, label ->
                                ControlChip(
                                    label = label,
                                    selected = config.quality == index,
                                    focused = false,
                                    onClick = { onUpdate(config.copy(quality = index)) }
                                )
                            }
                        }
                    }

                    ControlGroup("RESOLUTION") {
                        val scaleOptions = listOf(1.0f to "Native", 0.85f to "85%", 0.75f to "75%", 0.67f to "67%", 0.5f to "50%")
                        scaleOptions.forEach { (value, label) ->
                            ControlChip(
                                label = label,
                                selected = abs(config.renderScale - value) < 0.001f,
                                focused = false,
                                onClick = { onUpdate(config.copy(renderScale = value)) }
                            )
                        }
                    }
                }
            }
        }
    }
}

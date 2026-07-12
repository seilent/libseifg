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
import androidx.compose.ui.text.style.TextAlign
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

private val TealPrimary = Color(0xFF7FC8BA)
private val TealDim = Color(0xFF4A9E8E)
private val TealMuted = Color(0xFF2A5C53)
private val TealContainer = Color(0xFF1E3F3A)
private val SurfaceDark = Color(0xFF16191A)
private val BackgroundDark = Color(0xFF0F1112)
private val SurfaceCard = Color(0xFF1E2224)
private val SurfaceElevated = Color(0xFF252A2C)
private val OnSurfaceLight = Color(0xFFEAECED)
private val OnSurfaceMuted = Color(0xFF8A9196)
private val OutlineColor = Color(0xFF3A3F42)
private val OutlineVariantColor = Color(0xFF2C3133)

private val UnifiedShape = RoundedCornerShape(12.dp)

private val SefgColorScheme = darkColorScheme(
    primary = TealPrimary,
    onPrimary = Color(0xFF0A1F1B),
    primaryContainer = TealContainer,
    onPrimaryContainer = TealPrimary,
    secondary = TealDim,
    onSecondary = Color(0xFF0A1F1B),
    secondaryContainer = TealContainer,
    onSecondaryContainer = TealPrimary,
    tertiary = TealDim,
    onTertiary = Color(0xFF0A1F1B),
    tertiaryContainer = TealContainer,
    onTertiaryContainer = TealPrimary,
    surface = SurfaceDark,
    onSurface = OnSurfaceLight,
    surfaceVariant = SurfaceCard,
    onSurfaceVariant = OnSurfaceMuted,
    background = BackgroundDark,
    onBackground = OnSurfaceLight,
    outline = OutlineColor,
    outlineVariant = OutlineVariantColor
)

@Composable
fun Modifier.dpadFocusHighlight(focused: Boolean): Modifier {
    return if (focused) {
        this
            .border(3.dp, TealPrimary, UnifiedShape)
            .background(TealPrimary.copy(alpha = 0.08f), UnifiedShape)
    } else this
}

data class AppEntry(
    val label: String,
    val packageName: String,
    val icon: Drawable
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

data class AppConfig(
    var enabled: Boolean = false,
    var targetFps: Int = 60,
    var multiplier: Int = 2,
    var quality: Int = 0
)

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
                custom.put(pkg, entry)
            }
        }
        json.put("custom", custom)

        val cacheFile = File(cacheDir, "TargetList.json")
        cacheFile.writeText(json.toString(2))

        Shell.cmd(
            "cp ${cacheFile.absolutePath} /data/local/tmp/TargetList.json && chmod 644 /data/local/tmp/TargetList.json"
        ).exec()
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = SefgColorScheme) {
                Surface(modifier = Modifier.fillMaxSize(), color = BackgroundDark) {
                    ConfigScreen()
                }
            }
        }
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
                                val valid = validOutputs(refreshHz, effMult)
                                map[key] = AppConfig(
                                    enabled = true,
                                    targetFps = snapToNearest(targetFps, valid),
                                    multiplier = effMult,
                                    quality = quality
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
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "SeFG",
                style = MaterialTheme.typography.titleLarge.copy(
                    fontWeight = FontWeight.SemiBold,
                    letterSpacing = 0.5.sp
                ),
                color = TealPrimary
            )

            Spacer(Modifier.width(16.dp))

            if (searchActive) {
                OutlinedTextField(
                    value = searchQuery,
                    onValueChange = { searchQuery = it },
                    modifier = Modifier
                        .weight(1f)
                        .height(48.dp)
                        .focusRequester(searchFieldFocusRequester)
                        .onPreviewKeyEvent { event ->
                            if (event.type == KeyEventType.KeyDown && event.key == Key.Escape) {
                                searchActive = false
                                searchQuery = ""
                                keyboardController?.hide()
                                searchBoxFocusRequester.requestFocus()
                                true
                            } else if (event.type == KeyEventType.KeyDown && event.key == Key.Back) {
                                searchActive = false
                                searchQuery = ""
                                keyboardController?.hide()
                                searchBoxFocusRequester.requestFocus()
                                true
                            } else false
                        },
                    placeholder = { Text("Search apps", fontSize = 14.sp) },
                    leadingIcon = { Icon(Icons.Default.Search, contentDescription = null, modifier = Modifier.size(18.dp)) },
                    singleLine = true,
                    shape = UnifiedShape,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = TealPrimary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline,
                        cursorColor = TealPrimary
                    ),
                    textStyle = MaterialTheme.typography.bodyMedium
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
                        .height(48.dp)
                        .clip(UnifiedShape)
                        .dpadFocusHighlight(searchFocused)
                        .background(
                            if (searchFocused) SurfaceElevated else Color.Transparent,
                            UnifiedShape
                        )
                        .clickable(
                            interactionSource = searchInteraction,
                            indication = null
                        ) { searchActive = true }
                        .focusRequester(searchBoxFocusRequester)
                        .padding(horizontal = 14.dp),
                    contentAlignment = Alignment.CenterStart
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Default.Search,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                            tint = if (searchFocused) TealPrimary else MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Spacer(Modifier.width(10.dp))
                        Text(
                            if (searchQuery.isNotBlank()) searchQuery else "Search apps",
                            style = MaterialTheme.typography.bodyMedium,
                            color = if (searchQuery.isNotBlank()) MaterialTheme.colorScheme.onSurface
                            else MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            Spacer(Modifier.width(10.dp))

            val gearInteraction = remember { MutableInteractionSource() }
            val gearFocused by gearInteraction.collectIsFocusedAsState()

            Box(
                modifier = Modifier
                    .size(48.dp)
                    .clip(UnifiedShape)
                    .dpadFocusHighlight(gearFocused)
                    .background(
                        when {
                            advanced -> TealPrimary
                            gearFocused -> SurfaceElevated
                            else -> SurfaceElevated
                        },
                        UnifiedShape
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
                    modifier = Modifier.size(22.dp),
                    tint = if (advanced) Color.White else MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant, thickness = 0.5.dp)

        if (hasRoot == false) {
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                shape = UnifiedShape,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                Text(
                    "Root access unavailable. Cannot read or write config.",
                    modifier = Modifier.padding(14.dp),
                    color = MaterialTheme.colorScheme.onErrorContainer,
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }

        if (apps.isEmpty() && hasRoot != null) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = TealPrimary, strokeWidth = 3.dp)
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(vertical = 4.dp)
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
                    AppRow(
                        app = app,
                        config = cfg,
                        refreshHz = refreshHz,
                        advanced = advanced,
                        focusRequester = if (isFirst) firstItemFocusRequester else null,
                        onUpdate = { updated ->
                            configs = configs.toMutableMap().also { it[app.packageName] = updated }
                        }
                    )
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppRow(
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

    var rowFocused by remember { mutableStateOf(false) }

    val rowModifier = Modifier
        .padding(horizontal = 12.dp, vertical = 3.dp)
        .clip(UnifiedShape)
        .onFocusChanged { rowFocused = it.hasFocus }
        .dpadFocusHighlight(rowFocused)
        .background(if (config.enabled) SurfaceCard else Color.Transparent, UnifiedShape)
        .padding(horizontal = 12.dp, vertical = 8.dp)
        .let { mod -> if (focusRequester != null) mod.focusRequester(focusRequester) else mod }
        .focusGroup()

    Column(modifier = rowModifier) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Image(
                bitmap = app.icon.toBitmap(96, 96).asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.size(36.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    app.label,
                    style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium)
                )
                Text(
                    app.packageName,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 11.sp
                )
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onUpdate(config.copy(enabled = it)) },
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = TealPrimary,
                    uncheckedThumbColor = MaterialTheme.colorScheme.onSurfaceVariant,
                    uncheckedTrackColor = Color.Transparent,
                    uncheckedBorderColor = MaterialTheme.colorScheme.outline
                )
            )
        }

        AnimatedVisibility(visible = config.enabled) {
            Column(modifier = Modifier.padding(start = 48.dp, top = 6.dp, bottom = 4.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Multiplier", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(4.dp))
                        SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                            multiplierOptions.forEachIndexed { index, mult ->
                                SegmentedButton(
                                    selected = config.multiplier == mult,
                                    onClick = {
                                        val newValid = validOutputs(refreshHz, mult, minBase)
                                        val snapped = snapToNearest(config.targetFps, newValid)
                                        onUpdate(config.copy(multiplier = mult, targetFps = snapped))
                                    },
                                    shape = SegmentedButtonDefaults.itemShape(index, multiplierOptions.size, baseShape = UnifiedShape),
                                    icon = {},
                                    colors = SegmentedButtonDefaults.colors(
                                        activeContainerColor = TealContainer,
                                        activeContentColor = TealPrimary,
                                        inactiveContainerColor = Color.Transparent,
                                        inactiveContentColor = OnSurfaceMuted
                                    )
                                ) {
                                    Text("${mult}x", fontSize = 13.sp)
                                }
                            }
                        }
                    }

                    val outputs = validOutputs(refreshHz, config.multiplier, minBase).sorted()
                    Column(modifier = Modifier.weight(1.2f)) {
                        Text("Output FPS", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(4.dp))
                        if (outputs.size == 1) {
                            Box(
                                modifier = Modifier
                                    .height(40.dp)
                                    .widthIn(min = 56.dp)
                                    .border(1.dp, OutlineColor, UnifiedShape)
                                    .background(TealContainer, UnifiedShape)
                                    .padding(horizontal = 16.dp),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(
                                    "${outputs.first()}",
                                    fontSize = 13.sp,
                                    color = TealPrimary,
                                    fontWeight = FontWeight.Medium,
                                    textAlign = TextAlign.Center
                                )
                            }
                        } else {
                            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                                outputs.forEachIndexed { index, fps ->
                                    SegmentedButton(
                                        selected = config.targetFps == fps,
                                        onClick = { onUpdate(config.copy(targetFps = fps)) },
                                        shape = SegmentedButtonDefaults.itemShape(index, outputs.size, baseShape = UnifiedShape),
                                        icon = {},
                                        colors = SegmentedButtonDefaults.colors(
                                            activeContainerColor = TealContainer,
                                            activeContentColor = TealPrimary,
                                            inactiveContainerColor = Color.Transparent,
                                            inactiveContentColor = OnSurfaceMuted
                                        )
                                    ) {
                                        Text("$fps", fontSize = 13.sp)
                                    }
                                }
                            }
                        }
                    }

                    if (config.multiplier > 1) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text("Quality", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Spacer(Modifier.height(4.dp))
                            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                                qualityLabels.forEachIndexed { index, label ->
                                    SegmentedButton(
                                        selected = config.quality == index,
                                        onClick = { onUpdate(config.copy(quality = index)) },
                                        shape = SegmentedButtonDefaults.itemShape(index, qualityLabels.size, baseShape = UnifiedShape),
                                        icon = {},
                                        colors = SegmentedButtonDefaults.colors(
                                            activeContainerColor = TealContainer,
                                            activeContentColor = TealPrimary,
                                            inactiveContainerColor = Color.Transparent,
                                            inactiveContentColor = OnSurfaceMuted
                                        )
                                    ) {
                                        Text(label, fontSize = 12.sp)
                                    }
                                }
                            }
                        }
                    }
                }

                Spacer(Modifier.height(6.dp))
                val base = config.targetFps / config.multiplier
                Text(
                    "${config.targetFps} FPS out, cap $base",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 11.sp
                )
            }
        }
    }
}

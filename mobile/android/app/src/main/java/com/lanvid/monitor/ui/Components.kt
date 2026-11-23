package com.lanvid.monitor.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.pow
import kotlin.math.roundToInt

private const val FADER_MIN = -60f
private const val FADER_MAX = 12f
private const val TAPER = 1.6

private fun faderNorm(db: Float): Float =
    ((db - FADER_MIN) / (FADER_MAX - FADER_MIN)).coerceIn(0f, 1f).toDouble().pow(TAPER).toFloat()

private fun faderDb(n: Float): Float {
    val t = n.coerceIn(0f, 1f).toDouble().pow(1.0 / TAPER).toFloat()
    return ((FADER_MIN + t * (FADER_MAX - FADER_MIN)) * 10f).roundToInt() / 10f
}

/** Vertical fader in dB (-60..+12), perceptual taper; drag to move, double-tap = 0 dB. */
@Composable
fun VerticalFader(valueDb: Float, onChange: (Float) -> Unit, tint: Color, height: Dp = 190.dp) {
    val current by rememberUpdatedState(valueDb)
    val cb by rememberUpdatedState(onChange)
    Canvas(
        Modifier
            .width(44.dp)
            .height(height)
            .semantics { contentDescription = "Гучність"; stateDescription = "%.1f дБ".format(current) }
            .pointerInput(Unit) {
                detectVerticalDragGestures { change, _ ->
                    change.consume()
                    cb(faderDb(1f - change.position.y / size.height))
                }
            }
            .pointerInput(Unit) {
                detectTapGestures(
                    onDoubleTap = { cb(0f) },
                    onPress = { p -> cb(faderDb(1f - p.y / size.height)) },
                )
            }
    ) {
        val cx = size.width / 2f
        val y = size.height * (1f - faderNorm(current))
        drawRoundRect(Lv.cardHi, Offset(cx - 3.dp.toPx(), 0f), Size(6.dp.toPx(), size.height), CornerRadius(3.dp.toPx()))
        drawRoundRect(tint, Offset(cx - 3.dp.toPx(), y), Size(6.dp.toPx(), size.height - y), CornerRadius(3.dp.toPx()))
        val zero = size.height * (1f - faderNorm(0f))
        drawLine(Lv.muted, Offset(cx - 11.dp.toPx(), zero), Offset(cx + 11.dp.toPx(), zero), 1.dp.toPx())
        drawRoundRect(Lv.text, Offset(cx - 17.dp.toPx(), y - 8.dp.toPx()), Size(34.dp.toPx(), 16.dp.toPx()),
            CornerRadius(5.dp.toPx()))
        drawRect(tint, Offset(cx - 17.dp.toPx(), y - 1.dp.toPx()), Size(34.dp.toPx(), 2.dp.toPx()))
    }
}

/** Peak meter (-60..0 dBFS). */
@Composable
fun VuMeter(dbfs: Float, modifier: Modifier, vertical: Boolean = true) {
    Canvas(modifier) {
        val t = ((dbfs + 60f) / 60f).coerceIn(0f, 1f)
        drawRoundRect(Lv.cardHi, cornerRadius = CornerRadius(2.dp.toPx()))
        val brush = if (vertical) Brush.verticalGradient(listOf(Lv.red, Lv.amber, Lv.green))
                    else Brush.horizontalGradient(listOf(Lv.green, Lv.amber, Lv.red))
        if (vertical) {
            val h = size.height * t
            drawRoundRect(brush, Offset(0f, size.height - h), Size(size.width, h), CornerRadius(2.dp.toPx()))
        } else {
            drawRoundRect(brush, Offset.Zero, Size(size.width * t, size.height), CornerRadius(2.dp.toPx()))
        }
    }
}

/** Pan (-1 L .. +1 R) with a center detent. */
@Composable
fun PanControl(pan: Float, onChange: (Float) -> Unit, modifier: Modifier = Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Slider(
            value = pan,
            onValueChange = { v -> onChange(if (kotlin.math.abs(v) < 0.06f) 0f else (v * 100f).roundToInt() / 100f) },
            valueRange = -1f..1f,
            colors = SliderDefaults.colors(thumbColor = Lv.accent2, activeTrackColor = Lv.accent2, inactiveTrackColor = Lv.cardHi),
        )
        Text(
            when {
                pan == 0f -> "C"
                pan < 0f -> "L${(-pan * 100).roundToInt()}"
                else -> "R${(pan * 100).roundToInt()}"
            },
            color = Lv.muted, fontSize = 11.sp,
        )
    }
}

@Composable
fun PillToggle(label: String, on: Boolean, color: Color, onClick: () -> Unit) {
    Box(
        Modifier
            .size(width = 30.dp, height = 26.dp)
            .background(if (on) color else Lv.cardHi, RoundedCornerShape(7.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = if (on) Color.Black else Lv.text, fontWeight = FontWeight.Black, fontSize = 13.sp)
    }
}

/** Segmented chooser (pill buttons). */
@Composable
fun <T> SegmentRow(items: List<T>, selected: T, label: (T) -> String, onSelect: (T) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        items.forEach { it ->
            val sel = it == selected
            Box(
                Modifier
                    .weight(1f)
                    .heightIn(min = 34.dp)
                    .background(if (sel) Lv.accent else Lv.cardHi, RoundedCornerShape(9.dp))
                    .clickable { onSelect(it) }
                    .padding(vertical = 8.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(label(it), color = if (sel) Color.White else Lv.text, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
            }
        }
    }
}

/** Labeled parameter slider with value readout. */
@Composable
fun ParamSlider(
    title: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit,
    step: Float = 0.5f,
    format: String = "%.1f дБ",
) {
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth()) {
            Text(title, color = Lv.text, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Text(format.format(value), color = Lv.accent2, fontSize = 14.sp)
        }
        Slider(
            value = value,
            onValueChange = { v -> onChange(((v / step).roundToInt() * step).coerceIn(range.start, range.endInclusive)) },
            valueRange = range,
            colors = SliderDefaults.colors(thumbColor = Lv.accent, activeTrackColor = Lv.accent, inactiveTrackColor = Lv.cardHi),
        )
    }
}

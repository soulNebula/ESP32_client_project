// Kotlin
// 界面控件与配色
package com.smarthome.ble.ui

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

object Ui {

    // 色值三处要一致
    val BG = c(0x0F1720)
    val CARD = c(0x18222E)
    val CARD2 = c(0x1F2C3A)
    val LINE = c(0x2A3A4C)
    val TEXT = c(0xE8EEF5)
    val DIM = c(0x8FA3B8)
    val ACCENT = c(0x2F9E6E)
    val ACCENT2 = c(0x1F7A52)
    val DANGER = c(0xD94141)
    val DANGER2 = c(0xA92C2C)
    val WARN2 = c(0xA86512)
    val LAMP_ON = c(0xFFD21F)
    val LAMP_OFF = c(0x3A4A5C)

    fun c(rgb: Int): Int = (0xFF shl 24) or rgb

    fun dp(ctx: Context, v: Float): Int = (v * ctx.resources.displayMetrics.density + 0.5f).toInt()

    fun bgFill(fill: Int, radiusDp: Float, ctx: Context): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(ctx, radiusDp).toFloat()
            setColor(fill)
        }

    fun bgStroke(fill: Int, stroke: Int, radiusDp: Float, ctx: Context): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(ctx, radiusDp).toFloat()
            setColor(fill)
            setStroke(dp(ctx, 1f), stroke)
        }

    // 999f 出胶囊
    fun bgPill(fill: Int, ctx: Context): GradientDrawable = bgFill(fill, 999f, ctx)

    fun text(
        ctx: Context,
        content: String,
        sizeSp: Float,
        color: Int = TEXT,
        bold: Boolean = false,
        mono: Boolean = false,
    ): TextView = TextView(ctx).apply {
        text = content
        textSize = sizeSp
        setTextColor(color)
        if (bold) setTypeface(typeface, Typeface.BOLD)
        if (mono) typeface = Typeface.MONOSPACE
        includeFontPadding = false
    }

    fun vertical(ctx: Context): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
    }

    fun horizontal(ctx: Context): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    // 并排用 1f
    fun card(ctx: Context, weight: Float = 0f): LinearLayout = vertical(ctx).apply {
        background = bgStroke(CARD, LINE, 14f, ctx)
        val p = dp(ctx, 14f)
        setPadding(p, p, p, p)
        if (weight > 0f) {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight).apply {
                marginStart = dp(ctx, 6f)
                marginEnd = dp(ctx, 6f)
            }
        } else {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply {
                topMargin = dp(ctx, 12f)
            }
        }
    }

    fun button(
        ctx: Context,
        label: String,
        primary: Boolean,
        onClick: (View) -> Unit,
    ): TextView = text(ctx, label, 15f, TEXT, bold = true).apply {
        gravity = Gravity.CENTER
        background = if (primary) {
            bgStroke(ACCENT2, ACCENT, 10f, ctx)
        } else {
            bgStroke(CARD2, LINE, 10f, ctx)
        }
        val v = dp(ctx, 11f)
        setPadding(v, v, v, v)
        isClickable = true
        isFocusable = true
        // 要包 lambda
        setOnClickListener { v -> onClick(v) }
    }

    fun rowOfButtons(ctx: Context, vararg views: View): LinearLayout = horizontal(ctx).apply {
        for (v in views) {
            v.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = dp(ctx, 4f)
                marginEnd = dp(ctx, 4f)
            }
            addView(v)
        }
    }

    fun sectionTitle(ctx: Context, label: String): TextView = text(ctx, label, 13f, DIM, bold = true).apply {
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(ctx, 18f) }
    }

    fun kvLine(ctx: Context): TextView = text(ctx, "", 13f, DIM).apply {
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(ctx, 6f) }
    }

    // 输入框样式统一
    fun edit(ctx: Context, initial: String, hint: String, inputType: Int): EditText =
        EditText(ctx).apply {
            setText(initial)
            this.hint = hint
            setTextColor(TEXT)
            setHintTextColor(DIM)
            textSize = 15f
            this.inputType = inputType
            setSingleLine(true)
            background = bgStroke(c(0x0C141C), LINE, 9f, ctx)
            val p = dp(ctx, 10f)
            setPadding(p, p, p, p)
        }

    // 字段名加输入框
    fun field(ctx: Context, parent: LinearLayout, label: String, initial: String, hint: String, inputType: Int): EditText {
        parent.addView(text(ctx, label, 13f, DIM).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(ctx, 12f) }
        })
        val e = edit(ctx, initial, hint, inputType)
        parent.addView(e)
        return e
    }
}

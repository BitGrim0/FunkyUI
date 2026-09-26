// Layout — places leaves and containers inside the open container (see "Layout" in docs/ARCHITECTURE.md).
// Sizes along a container's main axis are known immediately for leaves; containers use last
// frame's content size at Begin and report their real size to the parent at End.

#include "Internal.h"

namespace Funky
{
    namespace
    {
        // Max* first, then Min*: the minimum wins when they conflict. 0 = no limit.
        float Limit(float value, float minimum, float maximum)
        {
            if (maximum > 0)
                value = Min(value, maximum);
            return Max(value, minimum);
        }

        // Desired size with the explicit Width/Height overrides and the limits applied (margins excluded).
        Vec2 Constrain(Vec2 desired, const LayoutSpec& spec)
        {
            return { Limit(spec.Width > 0 ? spec.Width : desired.X, spec.MinWidth, spec.MaxWidth),
                     Limit(spec.Height > 0 ? spec.Height : desired.Y, spec.MinHeight, spec.MaxHeight) };
        }

        Vec2 OuterSize(Vec2 size, const LayoutSpec& spec)
        {
            return { size.X + spec.Margin.Horizontal(), size.Y + spec.Margin.Vertical() };
        }

        float AlignmentFactor(HorizontalAlignment a)
        {
            return a == HorizontalAlignment::Center ? 0.5f : a == HorizontalAlignment::Right ? 1.0f : 0.0f;
        }

        float AlignmentFactor(VerticalAlignment a)
        {
            return a == VerticalAlignment::Center ? 0.5f : a == VerticalAlignment::Bottom ? 1.0f : 0.0f;
        }

        // The slot (margins included) of the container's next child of the given size.
        Rect NextSlot(const Container& c, Vec2 size, const LayoutSpec& spec)
        {
            Vec2 outer = OuterSize(size, spec);
            if (c.Kind == ContainerKind::Grid)
            {
                float width = c.ColumnWidth[c.Column];
                if (c.Columns[c.Column].Type == GridLength::Kind::Auto)
                    width = Max(width, outer.X); // an Auto column grows with this frame's cells
                return { c.Content.X + c.ColumnX[c.Column], c.Content.Y + c.RowY, width, Max(c.RowHeight, outer.Y) };
            }
            if (c.Direction == Orientation::Horizontal)
                return { c.Content.X + c.Cursor, c.Content.Y, outer.X, c.Content.Height };
            return { c.Content.X, c.Content.Y + c.Cursor, c.Content.Width, outer.Y };
        }

        // Final rect of a child of the given size inside its slot: margins, alignment, stretch.
        Rect Align(Rect slot, Vec2 size, const LayoutSpec& spec)
        {
            Rect area = slot.Deflate(spec.Margin);
            Rect r = { area.X, area.Y, size.X, size.Y };
            if (spec.HAlign == HorizontalAlignment::Stretch && spec.Width <= 0)
                r.Width = Limit(Max(area.Width, 0.0f), spec.MinWidth, spec.MaxWidth);
            else
                r.X += Max(area.Width - size.X, 0.0f) * AlignmentFactor(spec.HAlign);
            if (spec.VAlign == VerticalAlignment::Stretch && spec.Height <= 0)
                r.Height = Limit(Max(area.Height, 0.0f), spec.MinHeight, spec.MaxHeight);
            else
                r.Y += Max(area.Height - size.Y, 0.0f) * AlignmentFactor(spec.VAlign);
            return r;
        }

        Rect SnapToPixels(Rect r, float scale)
        {
            float left = Round(r.Left() * scale) / scale;
            float top = Round(r.Top() * scale) / scale;
            float right = Round(r.Right() * scale) / scale;
            float bottom = Round(r.Bottom() * scale) / scale;
            return { left, top, right - left, bottom - top };
        }

        // Grid columns from the Px / Auto (last frame's widest cell) / Star (share of the rest) definitions.
        void LayoutColumns(Container& c, const ContainerState* state)
        {
            float remaining = c.Content.Width - c.Spacing * float(c.ColumnCount - 1);
            float totalWeight = 0;
            for (uint32_t i = 0; i < c.ColumnCount; ++i)
            {
                const GridLength& column = c.Columns[i];
                float width = 0;
                if (column.Type == GridLength::Kind::Pixel)
                    width = column.Value;
                else if (column.Type == GridLength::Kind::Auto)
                    width = state ? state->ColumnWidths[i] : 0;
                else
                    totalWeight += column.Value;
                c.ColumnWidth[i] = width;
                remaining -= width;
            }

            float x = 0;
            for (uint32_t i = 0; i < c.ColumnCount; ++i)
            {
                if (c.Columns[i].Type == GridLength::Kind::Star && totalWeight > 0)
                    c.ColumnWidth[i] = Max(remaining, 0.0f) * c.Columns[i].Value / totalWeight;
                c.ColumnX[i] = x;
                x += c.ColumnWidth[i] + c.Spacing;
            }
        }

        // Closes the current grid row: remembers its height for the next frame and starts the next row.
        void FinishRow(UiImpl& ui, Container& c)
        {
            ContainerState* state = ui.ContainerStates.Find(c.Id);
            if (state && c.Row < MaxGridRows)
                state->RowHeights[c.Row] = c.RowMeasured;
            c.Measured.Y += (c.Row ? c.RowSpacing : 0) + c.RowMeasured;
            c.RowY += c.RowHeight + c.RowSpacing;
            ++c.Row;
            c.Column = 0;
            c.RowMeasured = 0;
            c.RowHeight = state ? state->RowHeights[Min(c.Row, MaxGridRows - 1)] : 0;
        }

        // Grid content width this frame: Px + widest Auto cells + Star columns wide enough for their
        // widest cell at their weight + spacing.
        float GridContentWidth(const Container& c)
        {
            float width = c.Spacing * float(c.ColumnCount - 1);
            float totalWeight = 0;
            float perWeight = 0;
            for (uint32_t i = 0; i < c.ColumnCount; ++i)
            {
                const GridLength& column = c.Columns[i];
                if (column.Type == GridLength::Kind::Pixel)
                    width += column.Value;
                else if (column.Type == GridLength::Kind::Auto)
                    width += c.ColumnMeasured[i];
                else if (column.Value > 0)
                {
                    totalWeight += column.Value;
                    perWeight = Max(perWeight, c.ColumnMeasured[i] / column.Value);
                }
            }
            return width + perWeight * totalWeight;
        }

        // Moves the container past a child of the given size; Measured tracks desired sizes.
        void Advance(UiImpl& ui, Container& c, Vec2 size, const LayoutSpec& spec)
        {
            Vec2 outer = OuterSize(size, spec);
            if (c.Kind == ContainerKind::Grid)
            {
                if (c.Columns[c.Column].Type == GridLength::Kind::Auto)
                    c.ColumnWidth[c.Column] = Max(c.ColumnWidth[c.Column], outer.X);
                c.ColumnMeasured[c.Column] = Max(c.ColumnMeasured[c.Column], outer.X);
                c.RowHeight = Max(c.RowHeight, outer.Y);
                c.RowMeasured = Max(c.RowMeasured, outer.Y);
                if (++c.Column == c.ColumnCount)
                    FinishRow(ui, c);
            }
            else if (c.Direction == Orientation::Horizontal)
            {
                c.Measured.X = c.Cursor + outer.X;
                c.Measured.Y = Max(c.Measured.Y, outer.Y);
                c.Cursor = c.Measured.X + c.Spacing;
            }
            else
            {
                c.Measured.X = Max(c.Measured.X, outer.X);
                c.Measured.Y = c.Cursor + outer.Y;
                c.Cursor = c.Measured.Y + c.Spacing;
            }
        }
    }

    Rect UiImpl::Place(Vec2 desired, const LayoutSpec& spec)
    {
        Vec2 size = Constrain(desired, spec);
        Container& c = Top();
        Rect rect = Align(NextSlot(c, size, spec), size, spec);
        Advance(*this, c, size, spec);
        return SnapToPixels(rect, Scale);
    }

    bool UiImpl::BeginContainer(ContainerKind kind, uint64_t id, const LayoutSpec& spec)
    {
        ContainerState* state = ContainerStates.FindOrAdd(id);
        if (!state)
            return false;
        state->LastFrame = FrameIndex;

        // Placed like a leaf of last frame's size; the parent advances by the real size at End.
        Vec2 size = Constrain(state->ContentSize, spec);
        Container c = {};
        c.Kind = kind;
        c.Direction = Orientation::Vertical;
        c.Id = id;
        c.Spec = spec;
        c.Outer = Align(NextSlot(Top(), size, spec), size, spec);
        c.Content = c.Outer;
        c.ClipIndex = CurrentClip;
        return OpenContainers.Push(c) != nullptr;
    }

    void UiImpl::EndContainer()
    {
        Container& c = Top();
        if (c.Kind == ContainerKind::Grid)
        {
            if (c.Column > 0)
                FinishRow(*this, c);
            c.Measured.X = GridContentWidth(c);
        }
        if (ContainerState* state = ContainerStates.Find(c.Id))
        {
            state->ContentSize = c.Measured;
            MemCopy(state->ColumnWidths, c.ColumnMeasured, sizeof(state->ColumnWidths));
        }

        Vec2 size = Constrain(c.Measured, c.Spec);
        LayoutSpec spec = c.Spec;
        OpenContainers.Pop();
        Advance(*this, Top(), size, spec);
        CurrentClip = Top().ClipIndex;
    }

    void Ui::EndScope()
    {
        UiImpl& ui = *Impl(this);
        FK_ASSERT(ui.OpenContainers.Count > 1); // the root is never closed by a scope
        if (ui.Top().Kind == ContainerKind::Panel)
            ui.EndPanel();
        else
            ui.EndContainer();
    }

    Scope Ui::Stack(const StackProps& props)
    {
        UiImpl& ui = *Impl(this);
        if (!ui.BeginContainer(ContainerKind::Stack, ui.MakeId(props.Key), ToLayoutSpec(props)))
            return Scope(nullptr, false);
        Container& c = ui.Top();
        c.Direction = props.Orientation;
        c.Spacing = props.Spacing;
        return Scope(this, true);
    }

    Scope Ui::Grid(const GridProps& props)
    {
        UiImpl& ui = *Impl(this);
        uint64_t id = ui.MakeId(props.Key);
        if (!ui.BeginContainer(ContainerKind::Grid, id, ToLayoutSpec(props)))
            return Scope(nullptr, false);

        Container& c = ui.Top();
        c.Spacing = props.ColumnSpacing;
        c.RowSpacing = props.RowSpacing;
        while (c.ColumnCount < MaxGridColumns && props.Columns[c.ColumnCount].Type != GridLength::Kind::Unused)
        {
            c.Columns[c.ColumnCount] = props.Columns[c.ColumnCount];
            ++c.ColumnCount;
        }
        if (c.ColumnCount == 0)
        {
            c.Columns[0] = Star();
            c.ColumnCount = 1;
        }

        const ContainerState* state = ui.ContainerStates.Find(id);
        LayoutColumns(c, state);
        c.RowHeight = state ? state->RowHeights[0] : 0;
        return Scope(this, true);
    }
}

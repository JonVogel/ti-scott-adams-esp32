"""
TI BASIC CALL CHAR() pattern editor.

Draw 8x8 characters, get 16-char hex strings suitable for
CALL CHAR(n, "...") in TI BASIC / Extended BASIC.

Left pane: main 8x8 edit grid for the currently selected character.
Middle pane: resizable rows x cols palette of characters. Click a
cell to load it into the edit grid; edits in the grid update the
palette cell live. Right pane: live hex listing for all characters.
"""

import json
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

CELL = 40
GRID = 8
PAD = 10

PALETTE_PIXEL = 6
PALETTE_CELL = GRID * PALETTE_PIXEL
PALETTE_GAP = 0

MAX_ROWS = 8
MAX_COLS = 8
LISTING_MAX_VISIBLE = 20

ON_COLOR = "#202020"
OFF_COLOR = "#f0f0f0"
GRID_COLOR = "#808080"
SELECT_COLOR = "#d83030"


class CharEditor:
    def __init__(self, root):
        self.root = root
        self.root.title("TI BASIC CALL CHAR() Editor")

        self.palette_rows = 4
        self.palette_cols = 4
        self.palette_count = self.palette_rows * self.palette_cols

        self.characters = [
            [[0] * GRID for _ in range(GRID)] for _ in range(self.palette_count)
        ]
        self.selected = 0
        self.paint_value = None
        self.palette_rects = []
        self.selection_highlight = None
        self.char_clipboard = None

        top = ttk.Frame(root)
        top.grid(row=0, column=0, padx=PAD, pady=PAD, sticky="nw")

        main_pane = ttk.Frame(top)
        main_pane.grid(row=0, column=0, padx=(0, PAD), sticky="n")

        canvas_size = CELL * GRID + 1
        self.canvas = tk.Canvas(
            main_pane,
            width=canvas_size,
            height=canvas_size,
            bg=OFF_COLOR,
            highlightthickness=0,
        )
        self.canvas.grid(row=0, column=0, columnspan=4, sticky="n")

        self.cells = [[None] * GRID for _ in range(GRID)]
        for r in range(GRID):
            for c in range(GRID):
                x0 = c * CELL
                y0 = r * CELL
                self.cells[r][c] = self.canvas.create_rectangle(
                    x0, y0, x0 + CELL, y0 + CELL,
                    fill=OFF_COLOR, outline=GRID_COLOR,
                )

        self.canvas.bind("<Button-1>", self.on_click)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)

        ttk.Label(main_pane, text="Hex:").grid(
            row=1, column=0, sticky="w", padx=(0, 4), pady=(PAD, 2)
        )
        self.hex_var = tk.StringVar(value="0000000000000000")
        self.hex_entry = ttk.Entry(
            main_pane,
            textvariable=self.hex_var,
            font=("Consolas", 12),
            width=18,
        )
        self.hex_entry.grid(row=1, column=1, columnspan=2, sticky="we", pady=(PAD, 2))
        self.hex_entry.bind("<Return>", self.on_hex_entered)
        self.hex_entry.bind("<FocusOut>", self.on_hex_entered)
        ttk.Button(main_pane, text="Copy", command=self.copy_hex).grid(
            row=1, column=3, sticky="we", padx=(4, 0), pady=(PAD, 2)
        )

        ttk.Button(main_pane, text="Clear", command=self.clear).grid(
            row=2, column=0, sticky="we", padx=(0, 2), pady=2
        )
        ttk.Button(main_pane, text="Invert", command=self.invert).grid(
            row=2, column=1, sticky="we", padx=2, pady=2
        )
        ttk.Button(main_pane, text="Flip H", command=self.flip_h).grid(
            row=2, column=2, sticky="we", padx=2, pady=2
        )
        ttk.Button(main_pane, text="Flip V", command=self.flip_v).grid(
            row=2, column=3, sticky="we", padx=(2, 0), pady=2
        )

        shift_frame = ttk.Frame(main_pane)
        shift_frame.grid(row=3, column=0, columnspan=4, pady=(4, 0))
        ttk.Label(shift_frame, text="Shift char:").pack(side="left", padx=(0, 4))
        ttk.Button(shift_frame, text="\u2190", width=3,
                   command=lambda: self.shift_current(-1, 0)).pack(side="left", padx=1)
        ttk.Button(shift_frame, text="\u2191", width=3,
                   command=lambda: self.shift_current(0, -1)).pack(side="left", padx=1)
        ttk.Button(shift_frame, text="\u2193", width=3,
                   command=lambda: self.shift_current(0, 1)).pack(side="left", padx=1)
        ttk.Button(shift_frame, text="\u2192", width=3,
                   command=lambda: self.shift_current(1, 0)).pack(side="left", padx=1)

        palette_frame = ttk.Frame(top)
        palette_frame.grid(row=0, column=1, sticky="n")

        size_frame = ttk.Frame(palette_frame)
        size_frame.grid(row=0, column=0, pady=(0, 4), sticky="w")
        ttk.Label(size_frame, text="Rows:").grid(row=0, column=0, padx=(0, 2))
        self.rows_var = tk.IntVar(value=self.palette_rows)
        ttk.Spinbox(
            size_frame, from_=1, to=MAX_ROWS, width=3,
            textvariable=self.rows_var, command=self.on_size_changed,
        ).grid(row=0, column=1, padx=(0, 8))
        ttk.Label(size_frame, text="Cols:").grid(row=0, column=2, padx=(0, 2))
        self.cols_var = tk.IntVar(value=self.palette_cols)
        ttk.Spinbox(
            size_frame, from_=1, to=MAX_COLS, width=3,
            textvariable=self.cols_var, command=self.on_size_changed,
        ).grid(row=0, column=3)

        self.palette = tk.Canvas(
            palette_frame,
            bg="white",
            highlightthickness=0,
        )
        self.palette.grid(row=1, column=0)
        self.palette.bind("<Button-1>", self.on_palette_click)

        self.selected_var = tk.StringVar(value="Editing char 0")
        ttk.Label(palette_frame, textvariable=self.selected_var).grid(
            row=2, column=0, pady=(5, 0)
        )
        ttk.Button(palette_frame, text="Copy Char", command=self.copy_char).grid(
            row=3, column=0, pady=(5, 0), sticky="we"
        )
        ttk.Button(palette_frame, text="Paste Char", command=self.paste_char).grid(
            row=4, column=0, sticky="we"
        )

        shift_all_frame = ttk.Frame(palette_frame)
        shift_all_frame.grid(row=5, column=0, pady=(8, 0))
        ttk.Label(shift_all_frame, text="Shift all").grid(row=0, column=0, columnspan=3)
        ttk.Button(shift_all_frame, text="\u2191", width=3,
                   command=lambda: self.shift_all(0, -1)).grid(row=1, column=1, pady=1)
        ttk.Button(shift_all_frame, text="\u2190", width=3,
                   command=lambda: self.shift_all(-1, 0)).grid(row=2, column=0, padx=1)
        ttk.Button(shift_all_frame, text="\u2192", width=3,
                   command=lambda: self.shift_all(1, 0)).grid(row=2, column=2, padx=1)
        ttk.Button(shift_all_frame, text="\u2193", width=3,
                   command=lambda: self.shift_all(0, 1)).grid(row=3, column=1, pady=1)

        listing_frame = ttk.Frame(top)
        listing_frame.grid(row=0, column=2, sticky="n", padx=(PAD, 0))
        ttk.Label(listing_frame, text="All hex codes").grid(
            row=0, column=0, columnspan=2, sticky="w"
        )
        self.listing = tk.Text(
            listing_frame,
            width=22,
            height=self.palette_count,
            font=("Consolas", 10),
            state="disabled",
            borderwidth=1,
            relief="solid",
        )
        self.listing.grid(row=1, column=0, sticky="n")
        listing_scroll = ttk.Scrollbar(
            listing_frame, orient="vertical", command=self.listing.yview
        )
        self.listing.config(yscrollcommand=listing_scroll.set)
        listing_scroll.grid(row=1, column=1, sticky="ns")
        self.listing.tag_configure("selected", background="#ffe0e0")

        bottom = ttk.Frame(root)
        bottom.grid(row=1, column=0, padx=PAD, pady=(0, 4), sticky="w")
        ttk.Button(bottom, text="Copy All", command=self.copy_all).pack(
            side="left", padx=(0, 4)
        )
        ttk.Button(bottom, text="Clear All", command=self.clear_all).pack(
            side="left", padx=4
        )
        ttk.Button(bottom, text="Save...", command=self.save_project).pack(
            side="left", padx=4
        )
        ttk.Button(bottom, text="Load...", command=self.load_project).pack(
            side="left", padx=(4, 0)
        )

        self.status_var = tk.StringVar(value="")
        ttk.Label(root, textvariable=self.status_var, foreground="#606060").grid(
            row=2, column=0, padx=PAD, pady=(0, PAD), sticky="w"
        )

        self.build_palette()
        self.update_selection_border()
        self.update_hex()

    @property
    def pixels(self):
        return self.characters[self.selected]

    def build_palette(self):
        self.palette.delete("all")
        self.palette_rects = [
            [[None] * GRID for _ in range(GRID)] for _ in range(self.palette_count)
        ]

        w = self.palette_cols * PALETTE_CELL
        h = self.palette_rows * PALETTE_CELL
        self.palette.config(width=w, height=h)

        for idx in range(self.palette_count):
            pr = idx // self.palette_cols
            pc = idx % self.palette_cols
            cx = pc * PALETTE_CELL
            cy = pr * PALETTE_CELL
            chars = self.characters[idx]
            for r in range(GRID):
                for c in range(GRID):
                    x = cx + c * PALETTE_PIXEL
                    y = cy + r * PALETTE_PIXEL
                    color = ON_COLOR if chars[r][c] else OFF_COLOR
                    self.palette_rects[idx][r][c] = self.palette.create_rectangle(
                        x, y, x + PALETTE_PIXEL, y + PALETTE_PIXEL,
                        fill=color, outline="",
                    )
        self.selection_highlight = self.palette.create_rectangle(
            0, 0, PALETTE_CELL, PALETTE_CELL,
            outline=SELECT_COLOR, width=2,
        )

    def on_size_changed(self):
        try:
            new_rows = int(self.rows_var.get())
            new_cols = int(self.cols_var.get())
        except (ValueError, tk.TclError):
            return
        new_rows = max(1, min(MAX_ROWS, new_rows))
        new_cols = max(1, min(MAX_COLS, new_cols))
        if new_rows == self.palette_rows and new_cols == self.palette_cols:
            return

        sel_r = self.selected // self.palette_cols
        sel_c = self.selected % self.palette_cols

        new_count = new_rows * new_cols
        new_chars = [[[0] * GRID for _ in range(GRID)] for _ in range(new_count)]
        for r in range(min(self.palette_rows, new_rows)):
            for c in range(min(self.palette_cols, new_cols)):
                old_idx = r * self.palette_cols + c
                new_idx = r * new_cols + c
                new_chars[new_idx] = self.characters[old_idx]
        self.characters = new_chars

        self.palette_rows = new_rows
        self.palette_cols = new_cols
        self.palette_count = new_count

        if sel_r < new_rows and sel_c < new_cols:
            self.selected = sel_r * new_cols + sel_c
        else:
            self.selected = 0
        self.redraw_main()

        self.build_palette()
        self.update_selection_border()
        self.update_hex()

    def cell_from_event(self, event):
        c = event.x // CELL
        r = event.y // CELL
        if 0 <= r < GRID and 0 <= c < GRID:
            return r, c
        return None

    def on_click(self, event):
        pos = self.cell_from_event(event)
        if pos is None:
            return
        r, c = pos
        self.paint_value = 1 - self.pixels[r][c]
        self.set_pixel(r, c, self.paint_value)
        self.update_hex()

    def on_drag(self, event):
        if self.paint_value is None:
            return
        pos = self.cell_from_event(event)
        if pos is None:
            return
        r, c = pos
        if self.pixels[r][c] != self.paint_value:
            self.set_pixel(r, c, self.paint_value)
            self.update_hex()

    def on_release(self, _event):
        self.paint_value = None

    def on_palette_click(self, event):
        pc = event.x // PALETTE_CELL
        pr = event.y // PALETTE_CELL
        if 0 <= pr < self.palette_rows and 0 <= pc < self.palette_cols:
            idx = pr * self.palette_cols + pc
            self.select(idx)

    def select(self, idx):
        if idx == self.selected:
            return
        self.selected = idx
        self.redraw_main()
        self.update_selection_border()
        self.update_hex()

    def set_pixel(self, r, c, value):
        self.pixels[r][c] = value
        color = ON_COLOR if value else OFF_COLOR
        self.canvas.itemconfig(self.cells[r][c], fill=color)
        self.palette.itemconfig(self.palette_rects[self.selected][r][c], fill=color)

    def redraw_main(self):
        for r in range(GRID):
            for c in range(GRID):
                color = ON_COLOR if self.pixels[r][c] else OFF_COLOR
                self.canvas.itemconfig(self.cells[r][c], fill=color)

    def redraw_palette_cell(self, idx):
        chars = self.characters[idx]
        for r in range(GRID):
            for c in range(GRID):
                color = ON_COLOR if chars[r][c] else OFF_COLOR
                self.palette.itemconfig(self.palette_rects[idx][r][c], fill=color)

    def update_selection_border(self):
        pr = self.selected // self.palette_cols
        pc = self.selected % self.palette_cols
        cx = pc * PALETTE_CELL
        cy = pr * PALETTE_CELL
        self.palette.coords(
            self.selection_highlight,
            cx, cy, cx + PALETTE_CELL, cy + PALETTE_CELL,
        )
        self.palette.tag_raise(self.selection_highlight)
        self.selected_var.set(f"Editing char {self.selected}")

    def char_to_hex(self, idx):
        chars = self.characters[idx]
        parts = []
        for r in range(GRID):
            byte = 0
            for c in range(GRID):
                if chars[r][c]:
                    byte |= 1 << (7 - c)
            parts.append(f"{byte:02X}")
        return "".join(parts)

    def update_hex(self):
        self.hex_var.set(self.char_to_hex(self.selected))
        self.status_var.set("")
        self.update_listing()

    def update_listing(self):
        visible = max(1, min(LISTING_MAX_VISIBLE, self.palette_count))
        self.listing.config(state="normal", height=visible)
        self.listing.delete("1.0", "end")
        for i in range(self.palette_count):
            self.listing.insert("end", f"{i:2d}: {self.char_to_hex(i)}\n")
        start = f"{self.selected + 1}.0"
        end = f"{self.selected + 1}.end"
        self.listing.tag_add("selected", start, end)
        self.listing.config(state="disabled")

    def hex_to_char(self, text):
        text = "".join(ch for ch in text.upper() if ch in "0123456789ABCDEF")
        text = text[:16].ljust(16, "0")
        char = [[0] * GRID for _ in range(GRID)]
        for r in range(GRID):
            byte = int(text[r * 2:r * 2 + 2], 16)
            for c in range(GRID):
                char[r][c] = 1 if byte & (1 << (7 - c)) else 0
        return char

    def on_hex_entered(self, _event=None):
        char = self.hex_to_char(self.hex_var.get().strip())
        self.characters[self.selected] = char
        self.hex_var.set(self.char_to_hex(self.selected))
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_listing()

    def copy_hex(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.hex_var.get())
        self.status_var.set(f"Copied: {self.hex_var.get()}")

    def copy_char(self):
        self.char_clipboard = [row[:] for row in self.pixels]
        self.status_var.set(f"Copied char {self.selected}")

    def paste_char(self):
        if self.char_clipboard is None:
            self.status_var.set("Nothing to paste — Copy Char first")
            return
        source = self.char_clipboard
        for r in range(GRID):
            for c in range(GRID):
                self.pixels[r][c] = source[r][c]
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()
        self.status_var.set(f"Pasted into char {self.selected}")

    def copy_all(self):
        lines = [self.char_to_hex(i) for i in range(self.palette_count)]
        self.root.clipboard_clear()
        self.root.clipboard_append("\n".join(lines))
        self.status_var.set(f"Copied {self.palette_count} hex strings")

    def clear(self):
        for r in range(GRID):
            for c in range(GRID):
                self.pixels[r][c] = 0
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()

    def clear_all(self):
        for idx in range(self.palette_count):
            for r in range(GRID):
                for c in range(GRID):
                    self.characters[idx][r][c] = 0
            self.redraw_palette_cell(idx)
        self.redraw_main()
        self.update_hex()

    def invert(self):
        for r in range(GRID):
            for c in range(GRID):
                self.pixels[r][c] ^= 1
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()

    def flip_h(self):
        for r in range(GRID):
            self.pixels[r].reverse()
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()

    def flip_v(self):
        self.pixels.reverse()
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()

    def shift_current(self, dx, dy):
        src = self.pixels
        n = GRID
        new = [[src[(r - dy) % n][(c - dx) % n] for c in range(n)] for r in range(n)]
        self.characters[self.selected] = new
        self.redraw_main()
        self.redraw_palette_cell(self.selected)
        self.update_hex()

    def shift_all(self, dx, dy):
        W = self.palette_cols * GRID
        H = self.palette_rows * GRID
        composite = [[0] * W for _ in range(H)]
        for idx in range(self.palette_count):
            pr = idx // self.palette_cols
            pc = idx % self.palette_cols
            chars = self.characters[idx]
            for r in range(GRID):
                for c in range(GRID):
                    composite[pr * GRID + r][pc * GRID + c] = chars[r][c]
        shifted = [
            [composite[(r - dy) % H][(c - dx) % W] for c in range(W)]
            for r in range(H)
        ]
        for idx in range(self.palette_count):
            pr = idx // self.palette_cols
            pc = idx % self.palette_cols
            for r in range(GRID):
                for c in range(GRID):
                    self.characters[idx][r][c] = shifted[pr * GRID + r][pc * GRID + c]
            self.redraw_palette_cell(idx)
        self.redraw_main()
        self.update_hex()

    def save_project(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("Char editor project", "*.json"), ("All files", "*.*")],
        )
        if not path:
            return
        data = {
            "version": 1,
            "rows": self.palette_rows,
            "cols": self.palette_cols,
            "characters": [self.char_to_hex(i) for i in range(self.palette_count)],
        }
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            messagebox.showerror("Save failed", str(e))
            return
        self.status_var.set(f"Saved: {path}")

    def load_project(self):
        path = filedialog.askopenfilename(
            filetypes=[("Char editor project", "*.json"), ("All files", "*.*")],
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            messagebox.showerror("Load failed", str(e))
            return

        try:
            rows = max(1, min(MAX_ROWS, int(data.get("rows", 4))))
            cols = max(1, min(MAX_COLS, int(data.get("cols", 4))))
            chars_hex = list(data.get("characters", []))
        except (TypeError, ValueError) as e:
            messagebox.showerror("Load failed", f"Bad project data: {e}")
            return

        count = rows * cols
        self.palette_rows = rows
        self.palette_cols = cols
        self.palette_count = count
        self.rows_var.set(rows)
        self.cols_var.set(cols)

        self.characters = []
        for i in range(count):
            if i < len(chars_hex):
                self.characters.append(self.hex_to_char(str(chars_hex[i])))
            else:
                self.characters.append([[0] * GRID for _ in range(GRID)])

        self.selected = 0
        self.build_palette()
        self.redraw_main()
        self.update_selection_border()
        self.update_hex()
        self.status_var.set(f"Loaded: {path}")


def main():
    root = tk.Tk()
    root.resizable(False, False)
    CharEditor(root)
    root.mainloop()


if __name__ == "__main__":
    main()

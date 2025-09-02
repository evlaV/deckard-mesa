from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor import TraceProcessor
from PIL import Image, ImageDraw, ImageColor, ImageTk
from tkinter import filedialog, messagebox
from tkinter.ttk import Treeview, Combobox
from ttkwidgets import CheckboxTreeview
import argparse, ast, logging, os, re, sys
import distinctipy
import inquirer
import itertools
import threading
import tkinter as tk
import time

logging.basicConfig(format='%(levelname)s: %(message)s')

WHITE = ImageColor.getrgb('white')
BLACK = ImageColor.getrgb('black')

from enum import IntEnum
class Stats(IntEnum):
    BIN_SIZE = 0
    BIN_COUNT = 1
    BINNED_AREA = 2
    FB_SIZE = 3
    FB_PER_BINNED_AREA_PERCENT = 4
    ORIG_BIN_GRID = 5
    ORIG_BIN_COUNT = 6
    BIN_PER_ORIG_BIN = 7
    BIGGEST_MERGED_BIN = 8

    def __str__(self):
        txt = ["Bin size",
               "Bin count",
               "Total binned area",
               "Framebuffer size",
               "FB area / Bin area",
               "Original bin grid",
               "Original bin count",
               "Final / Original bin count",
               "Biggest merged bins"]
        return txt[self.value]

    def print(stats_values):
        print("="*80)

        for (s, v) in zip(Stats, stats_values):
            print(f'{s}: {v}')

        print("="*80)

class TilingInfo:
    # Parameter names must match the Perfetto ones for the dictionary unpack to work
    def __init__(self, width, height, binWidth, binHeight, binCoordinates, perViewFDMScale):
        self.fb_size = (width, height)
        self.unit_bin_size = (binWidth, binHeight)
        self.bin_coords = binCoordinates
        self.bin_sizes = [(x2 - x1 + 1, y2 - y1 + 1) for (x1, y1), (x2, y2) in self.bin_coords]
        self.per_view_fdm_scales = perViewFDMScale

        def get_binned_area(bin_coords):
            max_x = 0
            max_y = 0
            for _, (x, y) in bin_coords:
                max_x = max(max_x, x)
                max_y = max(max_y, y)

            return (max_x, max_y)

        binned_area = get_binned_area(self.bin_coords)
        if self.fb_size[0] > binned_area[0] or self.fb_size[1] > binned_area[1]:
            raise ValueError(f'Framebuffer size {fb_size} exceeds binned area {binned_area}')

        self.binned_area = binned_area

    @property
    def view_count(self):
        return len(self.per_view_fdm_scales)

    def is_unit_bin(self, bin_size):
        return bin_size == self.unit_bin_size

    # Returns the amount of unit bins merged in the x and y direction.
    # E.g. (2, 2) indicates that 2x2 bins were merged into this bin
    def get_unit_bins(self, bin_size):
        def is_divisible(a, b):
            return a[0] % b[0] == 0 and b[1] % b[1] == 0

        if not is_divisible(bin_size, self.unit_bin_size):
            raise ValueError(f'Size {merged_tile} is not divisible by tile size {tile_size}')

        return (int(bin_size[0] / self.unit_bin_size[0]), int(bin_size[1] / self.unit_bin_size[1]))

    def get_stats(self):
        def area(val):
            return val[0] * val[1]

        fb_area_percent = (area(self.fb_size) / area(self.binned_area)) * 100
        original_grid_size = self.get_unit_bins((self.binned_area[0] + 1, self.binned_area[1] + 1))
        biggest_merged_tile = max([area(self.get_unit_bins(bin_size)) for bin_size in self.bin_sizes])

        stats_values = [None]*len(Stats)
        stats_values[Stats.BIN_SIZE] = self.unit_bin_size
        stats_values[Stats.BIN_COUNT] = len(self.bin_coords)
        stats_values[Stats.BINNED_AREA] = self.binned_area
        stats_values[Stats.FB_SIZE] = self.fb_size
        stats_values[Stats.FB_PER_BINNED_AREA_PERCENT] = f'{fb_area_percent:.2f} %'
        stats_values[Stats.ORIG_BIN_GRID] = original_grid_size
        stats_values[Stats.ORIG_BIN_COUNT] = area(original_grid_size)
        stats_values[Stats.BIN_PER_ORIG_BIN] = f'{len(self.bin_coords) / area(original_grid_size) * 100:.2f} %'
        stats_values[Stats.BIGGEST_MERGED_BIN] = biggest_merged_tile

        return stats_values

class DrawLayer(IntEnum):
    FRAMEBUFFER = 0
    VIEWPORT_0  = 1
    VIEWPORT_1  = 2
    VIEWPORT_2  = 3
    VIEWPORT_3  = 4
    VIEWPORT_4  = 5
    VIEWPORT_5  = 6
    VIEWPORT_6  = 7
    VIEWPORT_7  = 8
    VIEWPORT_8  = 9
    VIEWPORT_9  = 10
    VIEWPORT_10 = 11
    VIEWPORT_11 = 12
    VIEWPORT_12 = 13
    VIEWPORT_13 = 14
    VIEWPORT_14 = 15
    VIEWPORT_15 = 16
    SCISSOR_0   = 17
    SCISSOR_1   = 18
    SCISSOR_2   = 19
    SCISSOR_3   = 20
    SCISSOR_4   = 21
    SCISSOR_5   = 22
    SCISSOR_6   = 23
    SCISSOR_7   = 24
    SCISSOR_8   = 25
    SCISSOR_9   = 26
    SCISSOR_10  = 27
    SCISSOR_11  = 28
    SCISSOR_12  = 29
    SCISSOR_13  = 30
    SCISSOR_14  = 31
    SCISSOR_15  = 32

    def VIEWPORT(idx):
        vp = (DrawLayer.VIEWPORT_0, DrawLayer.VIEWPORT_1, DrawLayer.VIEWPORT_2, DrawLayer.VIEWPORT_3, DrawLayer.VIEWPORT_4, DrawLayer.VIEWPORT_5, DrawLayer.VIEWPORT_6, DrawLayer.VIEWPORT_7, DrawLayer.VIEWPORT_8, DrawLayer.VIEWPORT_9, DrawLayer.VIEWPORT_10, DrawLayer.VIEWPORT_11, DrawLayer.VIEWPORT_12, DrawLayer.VIEWPORT_13, DrawLayer.VIEWPORT_14, DrawLayer.VIEWPORT_15)

        return vp[idx]

    def SCISSOR(idx):
        sc = (DrawLayer.SCISSOR_0, DrawLayer.SCISSOR_1, DrawLayer.SCISSOR_2, DrawLayer.SCISSOR_3, DrawLayer.SCISSOR_4, DrawLayer.SCISSOR_5, DrawLayer.SCISSOR_6, DrawLayer.SCISSOR_7, DrawLayer.SCISSOR_8, DrawLayer.SCISSOR_9, DrawLayer.SCISSOR_10, DrawLayer.SCISSOR_11, DrawLayer.SCISSOR_12, DrawLayer.SCISSOR_13, DrawLayer.SCISSOR_14, DrawLayer.SCISSOR_15)

        return sc[idx]

class Draw:
    def __framebuffer(self, fb_size):
        assert not self.__layers[DrawLayer.FRAMEBUFFER]

        layer = Image.new('RGBA', self.img_size, color = (0, 0, 0, 75))
        ImageDraw.Draw(layer).rectangle([(0, 0), fb_size], fill=(0, 0, 0, 0), width=0)
        self.__layers[DrawLayer.FRAMEBUFFER] = layer

    def __init__(self, tile_info):
        self.img_size = (tile_info.binned_area[0] + 2, tile_info.binned_area[1] + 2)

        self.__enabled = [False]*max(DrawLayer)
        self.__layers = [None]*max(DrawLayer)
        self.__tile_info = tile_info
        self.__framebuffer(tile_info.fb_size)

    __SCALE = lambda component: int(255 * component)
    __DARKEN = lambda component: max(component - 75, 0)
    def __apply_color_op(color, op):
        c = []
        for component in color:
            c.append(op(component))

        return (c[0], c[1], c[2])

    def __xy_coloring(fdm_scale):
        if fdm_scale[0] == 0:
            assert fdm_scale[1] == 0
            return (1.0, 1.0, 1.0)

        return (1 / fdm_scale[0], 1 / fdm_scale[1], 0.0)

    def set_viewport_scissors(self, viewports, scissors):
        layer_count = len(viewports) + len(scissors)

        # TODO: Maybe color the layers on render, the first time when we know
        # how many total layers we have

        exclude_colors = [BLACK]

        for x_scale in (1, 2, 4, 8):
            for y_scale in (1, 2, 4, 8):
                exclude_colors.append(Draw.__xy_coloring((x_scale, y_scale)))

        colors = distinctipy.get_colors(layer_count * 2, exclude_colors, pastel_factor=0.25)
        color_idx = 0

        colors = [ Draw.__apply_color_op(c, Draw.__SCALE) for c in colors ]

        for (idx, vp) in enumerate(viewports):
            assert not self.__layers[DrawLayer.VIEWPORT(idx)]

            layer = Image.new('RGBA', self.img_size, color=(0, 0, 0, 0))
            layer_draw = ImageDraw.Draw(layer)
            layer_draw.rectangle(vp, fill=(0, 0, 0, 0), outline=colors[color_idx], width=5)

            ((x1, y1), (x2, y2)) = vp
            layer_draw.text(((x2 + x1) / 2, y1 + 10), f"Viewport {idx}", fill=colors[color_idx], anchor='mt', align='center', font_size=15)

            self.__layers[DrawLayer.VIEWPORT(idx)] = layer

            color_idx = color_idx + 1

        for (idx, sc) in enumerate(scissors):
            assert not self.__layers[DrawLayer.SCISSOR(idx)]

            layer = Image.new('RGBA', self.img_size, color = (0, 0, 0, 0))
            layer_draw = ImageDraw.Draw(layer)
            layer_draw.rectangle(sc, fill=(0, 0, 0, 0), outline=colors[color_idx], width=5)

            ((x1, y1), (x2, y2)) = sc
            layer_draw.text(((x2 + x1) / 2, y2 - 10), f"Scissor {idx}", fill=colors[color_idx], anchor='mb', align='center', font_size=15)

            self.__layers[DrawLayer.SCISSOR(idx)] = layer

            color_idx = color_idx + 1

    def disable_all_layers(self):
        for i in range(len(self.__enabled)):
            self.__enabled[i] = False

    def enable_layer(self, draw_layer):
        assert self.__layers[draw_layer]
        self.__enabled[draw_layer] = True

    def __draw_centered_multiline_text(d, center_coords, lines, font_sizes, color, spacing=5, top_padding=2):
        vertical_coords = []

        coords = (0, top_padding)
        for (line, font_size) in zip(lines, font_sizes):
            (l, t, r, b) = d.textbbox(coords, line, font_size=font_size, anchor='lt')
            b = b + spacing
            vertical_coords.append((t, b))
            coords = (0, b)

        total_height = vertical_coords[-1][1] - spacing
        coords = (center_coords[0], center_coords[1] - int(total_height / 2))
        
        for (line, font_size, (t, _)) in zip(lines, font_sizes, vertical_coords):
            coords = (coords[0], coords[1] + t)
            d.text(coords, line, font_size=font_size, fill=color, anchor='mt')

    def __render_view(self, view_idx):
        view = Image.new('RGB', self.img_size)
        view_draw = ImageDraw.Draw(view)

        tile_id = 0
        for (rect_coords, fdm_scale) in zip(self.__tile_info.bin_coords, self.__tile_info.per_view_fdm_scales[view_idx]):
            color = Draw.__xy_coloring(fdm_scale)
            color = Draw.__apply_color_op(color, Draw.__SCALE)

            # For darker bins make the text color white.
            text_color = distinctipy.get_text_color(Draw.__apply_color_op(color, Draw.__DARKEN))
            text_color = Draw.__apply_color_op(text_color, Draw.__SCALE)

            (x1, y1), (x2, y2) = rect_coords
            view_draw.rectangle([(x1, y1), (x2 + 1, y2 + 1)], outline=BLACK, fill=color)

            # Coordinate text
            bin_top_left = (x1 + 5, y1 + 5)
            view_draw.text(bin_top_left, f'({x1}, {y1})', font_size=15, fill=text_color)

            # Tile ID
            bin_bottom_left = (x1 + 5, y2 - 5)
            view_draw.text(bin_bottom_left, f'#{tile_id}', font_size=15, fill=text_color, anchor='lb')

            lines = []
            font_sizes = []
            bin_center = (int((x1 + x2) / 2), int((y1 + y2) / 2))

            # FDM scale
            (x_scale, y_scale) = fdm_scale
            lines.append(f'Scale: {x_scale} x {y_scale}')
            font_sizes.append(18)
            #view_draw.text(bin_center, f'Scale: {x_scale} x {y_scale}', font_size=18, fill=text_color, anchor='mm')

            lines.append(f'Bin size: {(x2 - x1 + 1, y2 - y1 + 1)}')
            font_sizes.append(14)

            Draw.__draw_centered_multiline_text(view_draw, bin_center, lines, font_sizes, text_color)

            tile_id = tile_id + 1

        return view

    def render(self, view_idx):
        logging.debug(f"Rendering View: {view_idx}")

        img = self.__render_view(view_idx)

        for (enabled, layer) in zip(self.__enabled, self.__layers):
            if not enabled:
                continue

            img.paste(layer, (0, 0), mask=layer)

        return img

    def close(self):
        for layer in self.__layers:
            if not layer:
                continue

            layer.close()

class BinLayoutTraceProcessor(TraceProcessor):
    __array_pattern = None

    def __create_tables(self):
        query = '''\
        -- Links cmd_buffers to appName
        CREATE TABLE cmd_buffers (
            id INT NOT NULL PRIMARY KEY,
            context_id INT,
            name STRING
        );

        -- NOTE: cmd_buffer_id could be reused when the cmd_buffer is freed and a
        -- new one is allocated. If the cmd_buffer_id gets reused with a different
        -- appName we'd get an error since we'd be trying to insert the
        -- cmd_buffer_id twice with a different appName.
        INSERT INTO cmd_buffers
        SELECT A2.int_value, A3.int_value, A1.string_value
        FROM args A1, args A2, args A3
        WHERE A1.arg_set_id=A2.arg_set_id AND
              A1.arg_set_id=A3.arg_set_id AND
              A3.arg_set_id=A2.arg_set_id AND
              A1.key='appName' AND
              A2.key='command_buffer' AND
              A3.key='context_id'
        GROUP BY A2.int_value, A1.string_value;

        -- Contains arg_set_id of all tiledRender events.
        CREATE TABLE binned_rps (
            arg_set_id INT PRIMARY KEY
        );

        INSERT INTO binned_rps
        SELECT arg_set_id
        FROM args
        WHERE key='tiledRender' AND string_value='true';

        -- Links arg_set, containing the binCoordinates, with cmd_buffers
        CREATE TABLE unique_binned_rps (
            arg_set_id INT PRIMARY KEY,
            cmd_buffer_id INT,
            context_id INT,
            FOREIGN KEY (cmd_buffer_id) REFERENCES cmd_buffers(id)
        );

        INSERT INTO unique_binned_rps
        SELECT DISTINCT A1.arg_set_id, cmd_buffers.id, cmd_buffers.context_id
        FROM args A1, args A2
        JOIN binned_rps ON binned_rps.arg_set_id=A1.arg_set_id AND
        binned_rps.arg_set_id=A2.arg_set_id
        JOIN cmd_buffers ON cmd_buffers.id=A2.int_value
        WHERE A1.key LIKE 'binCoordinates[_%]' AND
              A1.arg_set_id=A2.arg_set_id AND
              A2.key='command_buffer'
        GROUP BY A1.string_value;
        '''
        self.query(query)

    def __init__(self, on_trace_loaded=None, *args, **kwargs):
        if not os.path.isfile(kwargs['trace']):
            raise ValeuError(f"File not found, or not a file: {kwargs['trace']}")

        super().__init__(*args, **kwargs)
        if on_trace_loaded:
            on_trace_loaded()

        self.__create_tables()
        logging.debug("Created SQL tables")

        if not BinLayoutTraceProcessor.__array_pattern:
            BinLayoutTraceProcessor.__array_pattern = re.compile('\A(?#Match start of str)\w+(\[\d+\]){1,2}(?#Only up to 2D array)\Z(?#Match end of str)')


    def __query_into_list(self, query, field_name):
        return [getattr(row, field_name) for row in self.query(query)];

    def __query_table_into_dict(self, query):
        args = {}
        for row in self.query(query):
            if BinLayoutTraceProcessor.__array_pattern.match(row.key):
                literals = row.key.split('[')
                arr_name = literals[0]

                arr_idx = literals[1]
                arr_idx = int(arr_idx[:-1]) # Remove the ']'

                arr_idx_2 = None
                if len(literals) == 3:
                    arr_idx_2 = literals[2]
                    arr_idx_2 = int(arr_idx_2[:-1]) # Remove the ']'

                if not arr_name in args.keys():
                    args[arr_name] = []

                val = ast.literal_eval(row.string_value)

                if arr_idx_2 is None:
                    # Unordered array elements aren't supported since List.insert() ends
                    # up being the equivalent to List.append() when the index is greater
                    # than the length.
                    assert len(args[arr_name]) == arr_idx

                    args[arr_name].append(val)
                else:
                    if arr_idx_2 == 0:
                        assert len(args[arr_name]) == arr_idx
                        args[arr_name].append([])

                    # Not supporting unordered array elements.
                    assert len(args[arr_name][arr_idx]) == arr_idx_2
                    args[arr_name][arr_idx].append(val)

            else:
                args[row.key] = ast.literal_eval(row.string_value)

        return args

    def get_apps(self):
        query = '''\
        SELECT DISTINCT name
        FROM cmd_buffers
        JOIN unique_binned_rps ON unique_binned_rps.cmd_buffer_id=cmd_buffers.id;
        '''

        return self.__query_into_list(query, 'name')

    def get_context_ids(self, app_name):
        query = '''\
        SELECT DISTINCT context_id FROM cmd_buffers WHERE name='{}'
        '''.format(app_name)

        return [int(e) for e in self.__query_into_list(query, 'context_id')]

    def get_slice_ids(self, app_name, context_id):
        query = '''\
        SELECT slice.id
        FROM cmd_buffers
        JOIN unique_binned_rps ON unique_binned_rps.cmd_buffer_id=cmd_buffers.id
        JOIN slice USING(arg_set_id)
        WHERE cmd_buffers.name='{}' AND cmd_buffers.context_id={}
        GROUP BY arg_set_id;
        '''.format(app_name, context_id)

        slice_ids = [int(e) for e in self.__query_into_list(query, 'id')]
        assert len(slice_ids) > 0

        return slice_ids

    def get_tile_info(self, slice_id):
        query = f'''\
        SELECT key, string_value
        FROM args JOIN slice ON slice.arg_set_id=args.arg_set_id
        WHERE slice.id={slice_id} AND
              (args.key IN ('width', 'height', 'binWidth', 'binHeight') OR
               args.key LIKE 'binCoordinates[_%]' OR
               args.key LIKE 'perViewFDMScale[_%][_%]')
        '''

        args = self.__query_table_into_dict(query)
        return TilingInfo(**args)

    def get_viewports_and_scissors(self, slice_id):
        query = f'''\
        SELECT key, string_value
        FROM args JOIN slice USING(arg_set_id)
        WHERE slice.id={slice_id} AND
              (key LIKE 'viewports[_%]' OR key LIKE 'scissors[_%]')
        '''
        args = self.__query_table_into_dict(query)

        assert not args or len(args['viewports']) == len(args['scissors'])

        return args

class Console:
    trace_loaded = False
    animation_thread = None

    def select_item(items, item_name):
        if (len(items) == 1):
            logging.debug(f'Found just a single {item_name} with binning layout. {item_name}: {items[0]}')
            last_selection = items[0]
            yield items[0]

            return None

        last_selection = None;
        select = True
        while select:
            last_selection = inquirer.list_input(message=f'Select {item_name}', choices=items, default=last_selection, carousel=True)
            yield last_selection

            select = inquirer.confirm(f'Select another {item_name} ?')

        return None

    def trace_loading_animation(file_path):
        for c in itertools.cycle(['|', '/', '-', '\\']):
            if Console.trace_loaded:
                break

            print(f'\rLoading Perfetto trace: {file_path} {c}', end='', flush=True)
            time.sleep(0.1)
        print(f'\rLoading Perfetto trace: {file_path} - Complete', flush=True)

    def stop_animation():
        Console.trace_loaded = True
        Console.animation_thread.join()

    def main(file_path):
        Console.animation_thread = threading.Thread(target=Console.trace_loading_animation, args=[file_path], daemon=True)
        Console.animation_thread.start()

        with BinLayoutTraceProcessor(trace=file_path, on_trace_loaded=Console.stop_animation) as tp:
            apps_list = tp.get_apps()
            for app in Console.select_item(apps_list, 'app'):
                print(f'App: {app}')

                context_ids = tp.get_context_ids(app)
                if len(context_ids) > 1:
                    logging.info("Multiple context_id can arise due to applications having the same appName");

                for context_id in Console.select_item(context_ids, 'context_id (VkDevice on Turnip)'):
                    slice_ids = tp.get_slice_ids(app, context_id)

                    for slice_id in Console.select_item(slice_ids, 'slice_id'):
                        tiling_info = tp.get_tile_info(slice_id)
                        with tiling_info.new_image() as img:
                            img.show()
                        Stats.print(tiling_info.get_stats())

class App:
    def load_trace(self, file_path):
        def on_trace_loaded():
            logging.info(f"Loaded Perfetto Trace: {file_path}")

        try:
            tp = BinLayoutTraceProcessor(trace=file_path, on_trace_loaded=on_trace_loaded)
        except PerfettoException as e:
            messagebox.showerror("Error loading trace", str(e))
            logging.error(str(e))
            return False

        items = []
        # TODO: Sort. E.g. based on how much time each layout is used.
        # TODO: Do this in PerfettoSQL directly instead of loops here.
        for app in tp.get_apps():
            for context_id in tp.get_context_ids(app):
                for slice_id in tp.get_slice_ids(app, context_id):
                    items.append({'appName': app, 'context_id': context_id, 'slice_id': slice_id})

        if len(items) == 0:
            messagebox.showinfo("No binning layout", "No binning layouts found.")
            logging.info("No binning layouts found.")
            return False

        if self.__tp:
            self.__tp.close()
        self.__tp = tp

        self.__items = items

        return True

    def post_load_trace(self, ui, file_path):
        ui.set_file_path_in_title(file_path)
        ui.set_file_bar_path(file_path)
        ui.clear_canvas()
        ui.set_slice_selector_items(self.__items)

    def __vr_mode_possible(view_count, viewports, scissors):
        if view_count < 2:
            return False

        if len(viewports) < 2 or len(scissors) < 2:
            return False

        return True

    def on_slice_id_select(self, ui, slice_id):
        ui.set_slice_id_label(slice_id)

        tile_info = self.__tp.get_tile_info(slice_id)

        ui.set_stats(tile_info.get_stats())

        if self.__draw:
            self.__draw.close()
        self.__draw = Draw(tile_info)

        vp = self.__tp.get_viewports_and_scissors(slice_id)
        self.__draw.set_viewport_scissors(**vp)

        ui.delete_image_options()
        ui.append_image_option(DrawLayer.FRAMEBUFFER, 'Framebuffer')

        ui.append_image_option('vp', 'Viewports')
        for (idx, _) in enumerate(vp['viewports']):
            ui.append_image_option(DrawLayer.VIEWPORT(idx), f'Viewport {idx}', parent="vp")

        ui.append_image_option('sc', 'Scissors')
        for (idx, _) in enumerate(vp['scissors']):
            ui.append_image_option(DrawLayer.SCISSOR(idx), f'Scissor {idx}', parent="sc")

        ui.select_image_option_no_draw(DrawLayer.FRAMEBUFFER)
        ui.expand_image_options()

        ui.set_view_options(tile_info.view_count)

        ui.set_vr_mode_availability(App.__vr_mode_possible(tile_info.view_count, vp['viewports'], vp['scissors']))

        self.draw_view(ui, 0)

    def on_vr_mode_activation(self, ui):
        ui.clear_image_options_no_draw()
        ui.select_image_option_no_draw(DrawLayer.FRAMEBUFFER)
        ui.select_image_option_no_draw(DrawLayer.SCISSOR_0)

        self.draw_view(ui, 0, no_refresh=True)

        ui.clear_vr_image_options_no_draw()
        ui.select_vr_image_option_no_draw(DrawLayer.FRAMEBUFFER)
        ui.select_vr_image_option_no_draw(DrawLayer.SCISSOR_1)

        self.draw_vr_view(ui, 1)

    def draw_view(self, ui, view_idx, no_refresh=False):
        ui.set_canvas_image(self.__draw.render(view_idx))

        if no_refresh:
            return

        ui.refresh_canvas_image()

    def draw_vr_view(self, ui, view_idx):
        ui.set_canvas_vr_image(self.__draw.render(view_idx))
        ui.refresh_canvas_image()

    def clear_draw_layer(self):
        self.__draw.disable_all_layers()

    def set_draw_layer(self, option):
        self.__draw.enable_layer(option)

    def __init__(self, file_path=None):
        self.__tp = None
        self.__items = None
        self.__draw = None

        if file_path:
            self.load_trace(file_path)

    def close(self):
        logging.debug("Closing App")

        if self.__draw:
            self.__draw.close()
            self.__draw = None

        if self.__tp:
            self.__tp.close()
            self.__tp = None

class UI:
    ###########################################################################
    # Public UI functions
    ###########################################################################

    def set_file_path_in_title(self, file_path):
        title = self.__root.title().split(':')
        self.__root.title(f"{title[0]}: {file_path}")

    def set_file_bar_path(self, path):
        self.__file_bar_var.set(path)

    def set_slice_id_label(self, slice_id):
        self.__slice_id_var.set(f" Selected slice_id: {slice_id} ")

    # Each item should have 'appName', 'context_id', and 'slice_id'.
    def set_slice_selector_items(self, items):
        current_items = self.__slice_selector.get_children()
        for item in current_items:
            self.__slice_selector.delete(item)

        for item in items:
            self.__slice_selector.insert("", tk.END, text=item['appName'], values=(item['context_id'], item['slice_id']))

        self.__slice_selector.selection_add(self.__slice_selector.get_children()[0])

    def clear_canvas(self):
        self.__canvas.delete("all")
        if self.__image:
            self.__image.close()
            self.__image = None

        if self.__vr_image:
            self.__vr_image.close()
            self.__vr_image = None

    def refresh_canvas_image(self):
        EYE_PADDING = 4

        if not self.__image:
            return

        def resize_image(img, scale_factor):
            img_width, img_height = img.size
            new_size = (int(img_width * scale_factor), int(img_height * scale_factor))

            if new_size[0] <= 0 or new_size[1] <= 0:
                return None

            return img.resize(new_size, Image.Resampling.LANCZOS)

        c_width = self.__canvas.winfo_width()
        c_height = self.__canvas.winfo_height()

        if self.__vr_mode:
            c_width = (c_width - EYE_PADDING) / 2

        img_width, img_height = self.__image.size
        scale_factor = min(c_width / img_width, c_height / img_height)

        resized_image = resize_image(self.__image, scale_factor)
        if not resized_image:
            return

        # TODO: Close the resized_image somewhere?

        photo = ImageTk.PhotoImage(resized_image)

        center = (c_width / 2, c_height / 2)

        self.__canvas.delete("all")
        self.__canvas.create_image(center, image=photo, anchor=tk.CENTER)
        self.__canvas.image = photo

        if self.__vr_mode:
            resized_vr_image = resize_image(self.__vr_image, scale_factor)
            if not resized_vr_image:
                return

            photo = ImageTk.PhotoImage(resized_vr_image)

            # TODO: Close the resized_vr_image somewhere?

            center = (center[0] + c_width + EYE_PADDING, center[1])
            self.__canvas.create_image(center, image=photo, anchor=tk.CENTER)
            self.__canvas.image = [self.__canvas.image, photo]

            real_c_width = self.__canvas.winfo_width()
            real_c_height = self.__canvas.winfo_height()
            self.__canvas.create_line(real_c_width / 2 - (EYE_PADDING / 2), 0, real_c_width / 2 - (EYE_PADDING / 2), real_c_height, fill='white', width=EYE_PADDING)
            #self.__canvas.create_line(0, 0, 20000, 20000, fill='white', width=10)


    def set_canvas_image(self, new_image):
        if self.__image:
            self.__image.close()

        self.__image = new_image

    def set_canvas_vr_image(self, new_image):
        if self.__vr_image:
            self.__vr_image.close()

        self.__vr_image = new_image

    def set_vr_mode_availability(self, available):
        self.__vr_mode_button['state'] = tk.NORMAL if available else tk.DISABLED

    def set_view_options(self, view_count):
        self.__view_selector['values'] = [ str(i) for i in range(view_count) ]
        self.__view_selector.current(0)

        self.__vr_view_selector['values'] = [ str(i) for i in range(view_count) ]
        self.__vr_view_selector.current(1)

    def delete_image_options(self):
        current_items = self.__image_options.get_children()
        for item in current_items:
            self.__image_options.delete(item)

        current_items = self.__vr_image_options.get_children()
        for item in current_items:
            self.__vr_image_options.delete(item)

    def append_image_option(self, key, text, parent=""):
        self.__image_options.insert(parent, tk.END, key, text=text)
        self.__vr_image_options.insert(parent, tk.END, key, text=text)

    def clear_image_options_no_draw(self):
        for layer in self.__image_options.get_children():
            self.__image_options.change_state(layer, 'unchecked')
        self.app.clear_draw_layer()

    def clear_vr_image_options_no_draw(self):
        for layer in self.__vr_image_options.get_children():
            self.__vr_image_options.change_state(layer, 'unchecked')
        self.app.clear_draw_layer()

    def select_image_option_no_draw(self, draw_layer):
        self.__image_options.change_state(draw_layer, 'checked')
        # The above doesn't trigger the on select function so we have to handle
        # things ourselves.
        self.app.set_draw_layer(draw_layer)

    def select_vr_image_option_no_draw(self, draw_layer):
        self.__vr_image_options.change_state(draw_layer, 'checked')
        # The above doesn't trigger the on select function so we have to handle
        # things ourselves.
        self.app.set_draw_layer(draw_layer)

    def expand_image_options(self):
        self.__image_options.expand_all()
        self.__vr_image_options.expand_all()

    # the 'stats' list must be in the same order as the Stats enum
    def set_stats(self, stats):
        for (var, s) in zip(self.__stats_vars, stats):
            var.set(str(s))

    ###########################################################################
    # Event Handlers
    ###########################################################################

    def __on_load_button_click(self):
        file_path = None

        while not file_path:
            # TODO: Use the native file picker instead of the Tk one.
            # TODO: Test what happens if an invalid file type is selected
            file_path = filedialog.askopenfilename()

            # In case the operation is canceled by the user.
            if len(file_path) == 0:
                return

            ret = self.app.load_trace(file_path)
            if not ret:
                file_path = None

        self.app.post_load_trace(self, file_path)

    def __on_export_button_click(self):
        file_path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[('PNG', '*.png')])
        if not file_path:
            return

        self.__image.save(file_path)

    def __on_vr_export_button_click(self):
        file_path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[('PNG', '*.png')])
        if not file_path:
            return

        self.__vr_image.save(file_path)

    def __on_file_bar_write(self, event):
        file_path = event.widget.get()
        if not os.path.isfile(file_path):
            messagebox.showerror("File not found", f'File not found, or not a file: {file_path}')
            return

        ret = self.app.load_trace(file_path)
        if not ret:
            return

        self.app.post_load_trace(self, file_path)

    def __on_slice_selector_item_select(self, event):
        # TODO: Handle same item being selected over and over
        item = event.widget.selection()
        if item == ():
            return

        slice_id = event.widget.item(item[0], option="values")[1]

        self.app.on_slice_id_select(self, slice_id)

    def __on_vr_mode_toggle(self):
        if self.__vr_mode_var.get():
            assert not self.__vr_mode
            self.__vr_mode = True
            self.__vr_frame.pack(expand=True, fill=tk.BOTH)
            # Update view selectors to avoid outdated view indices. This can
            # happen if VR mode is selected after having selected a non 0 view
            # in the normal mode. For the VR view index, it can happen if the VR
            # mode is toggled multiple times and the view index was changed.
            self.__view_selector.current(0)
            self.__vr_view_selector.current(1)
            self.app.on_vr_mode_activation(self)
        else:
            assert self.__vr_mode
            self.__vr_mode = False
            self.__vr_frame.pack_forget()

        self.refresh_canvas_image()

    def __on_view_select(self, event):
        view_idx = event.widget.current()
        self.app.draw_view(self, view_idx)

    def __on_vr_view_select(self, event):
        view_idx = event.widget.current()
        self.app.draw_vr_view(self, view_idx)

    def __on_image_option_select(self, event):
        view_idx = self.__view_selector.current()

        # TODO: On opening a drop down this gets called again so maybe early
        # return if the draw options haven't changed
        selection_list = event.widget.get_checked()
        if not selection_list:
            self.app.clear_draw_layer()
            self.app.draw_view(self, view_idx)
            return

        self.app.clear_draw_layer()
        for item in selection_list:
            self.app.set_draw_layer(DrawLayer(int(item)))

        self.app.draw_view(self, view_idx)

    def __on_vr_image_option_select(self, event):
        view_idx = self.__vr_view_selector.current()

        # TODO: On opening a drop down this gets called again so maybe early
        # return if the draw options haven't changed
        selection_list = event.widget.get_checked()
        if not selection_list:
            self.app.clear_draw_layer()
            self.app.draw_vr_view(self, view_idx)
            return

        self.app.clear_draw_layer()
        for item in selection_list:
            self.app.set_draw_layer(DrawLayer(int(item)))

        self.app.draw_vr_view(self, view_idx)

    ###########################################################################
    # UI setup
    ###########################################################################

    def __file_bar_frame(self, parent_frame, on_load_button_click, on_file_bar_write):
        frame = tk.Frame(parent_frame, borderwidth=1, relief=tk.RAISED)

        inner_frame = tk.Frame(frame)

        load_button = tk.Button(inner_frame, text="Load Perfetto Trace", command=on_load_button_click)
        load_button.pack(side=tk.LEFT)

        self.__file_bar_var = tk.StringVar()
        value_entry = tk.Entry(inner_frame, textvariable=self.__file_bar_var)
        value_entry.bind('<Return>', on_file_bar_write)
        value_entry.pack(expand=True, side=tk.RIGHT, anchor=tk.W, fill=tk.X)

        inner_frame.pack(padx=5, pady=5, expand=True, fill=tk.BOTH)

        return frame

    def __stats_display(self, parent_frame):
        self.__stats_vars = [None] * len(Stats)

        frame = tk.Frame(parent_frame)

        for stat in Stats:
            stat_entry = tk.Entry(frame, bd=0, highlightthickness=0)
            stat_entry.insert(0, stat)
            stat_entry.configure(state='readonly')
            stat_entry.grid(padx=5, row=stat.value, column=0)

            value_var = tk.StringVar()
            value_var.set("-")
            value_entry = tk.Entry(frame, textvariable=value_var, bd=0, highlightthickness=0, state="readonly", justify="right")
            value_entry.grid(padx=5, row=stat.value, column=1, sticky=tk.E)

            self.__stats_vars[stat.value] = value_var

        # This is what makes the values move together with the PanedWindow
        frame.grid_columnconfigure(1, weight=2)

        return frame

    def __slice_selector_frame(self, parent_frame, on_select_callback):
        self.__slice_id_var = tk.StringVar()
        self.__slice_id_var.set(" Select binning layout ")
        selection_label = tk.Label(textvariable=self.__slice_id_var)

        frame = tk.LabelFrame(parent_frame, labelwidget=selection_label)

        slice_selector_frame = tk.Frame(frame)
        slice_selector_frame.pack(padx=3, pady=3, expand=True, side=tk.BOTTOM, fill=tk.BOTH)

        self.__slice_selector = Treeview(slice_selector_frame, columns=('context_id', 'slice_id'), selectmode=tk.BROWSE)

        self.__slice_selector.heading('#0', text="appName")
        self.__slice_selector.heading('context_id', text="context_id")
        self.__slice_selector.heading('slice_id', text="slice_id")

        self.__slice_selector.column('#0', width=100)
        self.__slice_selector.column('context_id', width=100)
        self.__slice_selector.column('slice_id', width=100)

        self.__slice_selector.bind('<<TreeviewSelect>>', on_select_callback)
        self.__slice_selector.pack(expand=True, side=tk.LEFT, fill=tk.BOTH)

        scrollbar = tk.Scrollbar(slice_selector_frame, orient=tk.VERTICAL, command=self.__slice_selector.yview)
        self.__slice_selector['yscrollcommand'] = scrollbar.set
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        return frame

    def __right_panel(self, parent_frame):
        right_panel = tk.Frame(parent_frame)

        self.__vr_mode_var = tk.IntVar()
        self.__vr_mode_button = tk.Checkbutton(right_panel, text="VR Mode", anchor=tk.W, state=tk.DISABLED, command=self.__on_vr_mode_toggle, variable=self.__vr_mode_var)
        self.__vr_mode_button.pack(fill=tk.BOTH)

        export_button = tk.Button(right_panel, text="Export Image", command=self.__on_export_button_click)
        export_button.pack(fill=tk.BOTH, padx=5, pady=5)

        view_selector_frame = tk.Frame(right_panel)

        view_label = tk.Label(view_selector_frame, text='View :')
        view_label.pack(side=tk.LEFT, padx=5)

        self.__view_selector = Combobox(view_selector_frame, exportselection=0)
        self.__view_selector.bind('<<ComboboxSelected>>', self.__on_view_select)
        self.__view_selector.pack(side=tk.LEFT, fill=tk.BOTH, padx=5, pady=5)

        view_selector_frame.pack(fill=tk.BOTH, padx=5, pady=5)

        self.__image_options = CheckboxTreeview(right_panel)
        self.__image_options.bind('<<TreeviewSelect>>', self.__on_image_option_select)
        self.__image_options.heading('#0', text="Options")
        self.__image_options.pack(expand=True, fill=tk.BOTH)

        self.__vr_frame = tk.Frame(right_panel)

        vr_view_selector_frame = tk.Frame(self.__vr_frame)

        export_button = tk.Button(vr_view_selector_frame, text="Export Image", command=self.__on_vr_export_button_click)
        export_button.pack(fill=tk.BOTH, padx=5, pady=5)

        view_label = tk.Label(vr_view_selector_frame, text='View :')
        view_label.pack(side=tk.LEFT, padx=5)

        self.__vr_view_selector = Combobox(vr_view_selector_frame, exportselection=0)
        self.__vr_view_selector.bind('<<ComboboxSelected>>', self.__on_vr_view_select)
        self.__vr_view_selector.pack(side=tk.LEFT, fill=tk.BOTH, padx=5, pady=5)

        vr_view_selector_frame.pack(fill=tk.BOTH, padx=5, pady=5)

        self.__vr_image_options = CheckboxTreeview(self.__vr_frame)
        self.__vr_image_options.bind('<<TreeviewSelect>>', self.__on_vr_image_option_select)
        self.__vr_image_options.heading('#0', text="Options")
        self.__vr_image_options.pack(expand=True, fill=tk.BOTH)

        #self.__vr_frame.pack(expand=True, fill=tk.BOTH)
        self.__vr_frame.pack_forget()

        return right_panel

    def __init__(self, file_path=None):
        self.__image = None
        self.__vr_image = None

        self.app = App(file_path)

        self.__root = tk.Tk()
        self.__root.title("Binning Layout Visualizer")
        self.__root.config()

        file_bar = self.__file_bar_frame(self.__root, self.__on_load_button_click, self.__on_file_bar_write)
        file_bar.pack(fill=tk.X)

        paned_window = tk.PanedWindow(self.__root)

        left_panel = tk.Frame(paned_window)

        stats_display = self.__stats_display(left_panel)
        stats_display.pack(pady=10, fill=tk.BOTH)

        slice_selector_frame = self.__slice_selector_frame(left_panel, self.__on_slice_selector_item_select)
        slice_selector_frame.pack(expand=True, fill=tk.BOTH)

        paned_window.add(left_panel, padx=10, pady=10) 

        right_frame = tk.Frame(paned_window)

        self.__canvas = tk.Canvas(right_frame, width=1024, height=1024, background='black')

        self.__canvas.bind("<Configure>", lambda e: self.refresh_canvas_image())
        self.__canvas.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)

        right_panel = self.__right_panel(right_frame)
        right_panel.pack(side=tk.RIGHT, fill=tk.BOTH)
        self.__vr_mode = False

        paned_window.add(right_frame)

        paned_window.pack(expand=True, fill=tk.BOTH)
        self.__root.protocol('WM_DELETE_WINDOW', self.close)

        if file_path:
            self.app.post_load_trace(self, file_path)

    def close(self):
        logging.debug("Closing UI")

        self.app.close()

        if self.__image:
            self.__image.close()
            self.__image = None

        if self.__vr_image:
            self.__vr_image.close()
            self.__vr_image = None

        self.__root.destroy()

    def mainloop(self):
        self.__root.mainloop()

parser = argparse.ArgumentParser(prog='Bin Layout Visualizer',
                                 description="Visualize binning layouts from Perfetto captures.")
parser.add_argument("-c", "--console", action='store_true', help="No graphical interface, print the stats to stdout and display the layout.")
parser.add_argument("-f", "--file", help="Perfetto trace file.")
args = parser.parse_args()

if args.file:
    if not os.path.isfile(args.file):
        logging.error(f'File not found, or not a file: {file_path}')
        exit(1)

    args.file = os.path.abspath(args.file)

if args.console:
    if not args.file:
        logging.error('Trace file is required when using the console mode')
        exit(1)

    Console.main(args.file)
else:
    ui = UI(args.file)
    ui.mainloop()

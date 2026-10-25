"""Control-coordinate dimensions, with the documented legacy 416 fallback."""
import math


def axis_dimension(row, axis):
    name = 'width' if axis == 'x' else 'height'
    for key in ('coordinate_' + name, 'model_' + name):
        if key in row:
            try:
                value = float(row[key])
            except (ValueError, TypeError) as error:
                raise ValueError(f'invalid {key}: expected a finite positive dimension') from error
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f'invalid {key}: expected a finite positive dimension')
            return value
    return 416.0


def center(row, axis):
    return axis_dimension(row, axis) / 2.0

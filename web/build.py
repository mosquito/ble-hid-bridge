#!/usr/bin/env python3
"""Build web UI - outputs HTML to stdout.

Parses Vue 3 SFC-like components with <template> and <script> sections,
generates component registrations, and assembles final HTML.
"""
import re
import sys
from pathlib import Path

WEB_DIR = Path(__file__).parent


def escape_template_for_js(template: str) -> str:
    """Escape template string for use in JS template literal."""
    return (template
            .replace('\\', '\\\\')
            .replace('`', '\\`')
            .replace('${', '\\${'))


def extract_section(content: str, tag: str) -> str:
    """Extract content between <tag> and </tag>."""
    pattern = rf'<{tag}>(.*?)</{tag}>'
    match = re.search(pattern, content, re.DOTALL)
    return match.group(1).strip() if match else ''


def parse_component(file_path: Path) -> dict:
    """Parse SFC-like component file.

    Returns dict with 'name', 'template', and 'script' keys.
    """
    content = file_path.read_text()
    name = file_path.stem  # e.g., 'status-card'

    template = extract_section(content, 'template')
    script = extract_section(content, 'script')

    # If no <template> tag, treat entire content as template (backwards compat)
    if not template:
        template = content.strip()

    return {
        'name': name,
        'template': template,
        'script': script
    }


def generate_component_registration(comp: dict) -> str:
    """Generate Vue component registration code."""
    name = comp['name']
    template_escaped = escape_template_for_js(comp['template'])
    script = comp['script'].strip()

    if script:
        # Script should be an object expression: ({ name: '...', setup() {...} })
        # We merge our template into it
        return f'''app.component('{name}', {{
  template: `{template_escaped}`,
  ...{script}
}});'''
    else:
        # No script - simple template-only component
        return f'''app.component('{name}', {{
  template: `{template_escaped}`
}});'''


def build():
    # Read base files
    base = (WEB_DIR / 'base.html').read_text()
    styles = (WEB_DIR / 'styles.css').read_text()
    vue_js = (WEB_DIR / 'vue.min.js').read_text()
    app_js = (WEB_DIR / 'app.js').read_text()

    # Parse all components
    components = []
    components_dir = WEB_DIR / 'components'
    for f in sorted(components_dir.glob('*.html')):
        components.append(parse_component(f))

    # Generate component registrations
    registrations = '\n\n'.join(
        generate_component_registration(c) for c in components
    )

    # Insert registrations before app.mount()
    # Look for the mount call and insert registrations before it
    mount_pattern = r"(app\.mount\('#app'\);)"
    if re.search(mount_pattern, app_js):
        app_js_final = re.sub(
            mount_pattern,
            f'// Component registrations\n{registrations}\n\n\\1',
            app_js
        )
    else:
        # Fallback: append registrations at the end
        app_js_final = app_js + '\n\n' + registrations

    # Simple substitution for base template
    result = base
    result = result.replace('$vue_js', vue_js)
    result = result.replace('$styles', styles)
    result = result.replace('$app_js', app_js_final)
    
    sys.stdout.write(result)


if __name__ == '__main__':
    build()

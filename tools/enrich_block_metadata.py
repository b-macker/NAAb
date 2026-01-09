#!/usr/bin/env python3
"""
Enhanced Block Metadata Enrichment Script (Phase 1.4)

Analyzes block JSON files and enriches them with AI-powered discovery metadata:
- description, short_desc
- input_types, output_type
- keywords, use_cases, related_blocks
- Performance metrics (avg_execution_ms, max_memory_mb, performance_tier)
- Quality metrics (success_rate_percent, avg_tokens_saved, test_coverage_percent, security_audited, stability)

Usage:
    python3 enrich_block_metadata.py <blocks_directory>
    python3 enrich_block_metadata.py --single <block_file.json>
    python3 enrich_block_metadata.py --test 100  # Test on 100 random blocks
"""

import json
import os
import sys
import re
from pathlib import Path
from typing import Dict, List, Optional, Any
import random


class BlockMetadataEnricher:
    """Enriches block metadata with AI-discovery fields"""

    def __init__(self):
        self.language_patterns = {
            'cpp': {
                'function': r'(?:void|int|double|string|auto|bool)\s+(\w+)\s*\(',
                'return_type': r'^(void|int|double|string|auto|bool|float|char)',
                'keywords_from_includes': r'#include\s*[<"](\w+)',
            },
            'javascript': {
                'function': r'function\s+(\w+)\s*\(|const\s+(\w+)\s*=\s*\(',
                'return_type': r'return\s+',
                'keywords_from_requires': r'require\(["\'](\w+)',
            },
            'python': {
                'function': r'def\s+(\w+)\s*\(',
                'return_type': r'->\s*(\w+)',
                'keywords_from_imports': r'import\s+(\w+)|from\s+(\w+)',
            },
        }

    def enrich_block(self, block_data: Dict[str, Any]) -> Dict[str, Any]:
        """
        Enrich a single block with enhanced metadata

        Args:
            block_data: Original block JSON data

        Returns:
            Enriched block data with all new fields
        """
        code = block_data.get('code', '')
        language = block_data.get('language', 'unknown')
        block_id = block_data.get('id', 'UNKNOWN')
        name = block_data.get('name', block_id)

        # Generate description from code analysis
        description = self._generate_description(code, name, language)
        short_desc = self._generate_short_desc(description, name)

        # Extract type information
        input_types, output_type = self._extract_types(code, language)

        # Extract keywords from code
        keywords = self._extract_keywords(code, name, language)

        # Generate use cases
        use_cases = self._generate_use_cases(code, name, language)

        # Find related blocks (placeholder - would require database access)
        related_blocks = self._find_related_blocks(block_id, keywords, language)

        # Estimate performance metrics
        perf_metrics = self._estimate_performance(code, language)

        # Quality metrics (defaults for now)
        quality_metrics = self._default_quality_metrics()

        # Add all enriched fields to block data
        enriched = block_data.copy()
        enriched.update({
            'description': description,
            'short_desc': short_desc,
            'input_types': input_types,
            'output_type': output_type,
            'keywords': keywords,
            'use_cases': use_cases,
            'related_blocks': related_blocks,
            'avg_execution_ms': perf_metrics['avg_execution_ms'],
            'max_memory_mb': perf_metrics['max_memory_mb'],
            'performance_tier': perf_metrics['performance_tier'],
            'success_rate_percent': quality_metrics['success_rate_percent'],
            'avg_tokens_saved': quality_metrics['avg_tokens_saved'],
            'test_coverage_percent': quality_metrics['test_coverage_percent'],
            'security_audited': quality_metrics['security_audited'],
            'stability': quality_metrics['stability'],
        })

        return enriched

    def _generate_description(self, code: str, name: str, language: str) -> str:
        """Generate detailed description from code analysis"""
        if not code:
            return f"Block {name} for {language}"

        # Extract function names
        functions = self._extract_function_names(code, language)

        # Look for comments
        comments = self._extract_comments(code, language)

        if comments:
            # Use first comment as base description
            desc = comments[0].strip()
        elif functions:
            # Generate from function names
            func_list = ', '.join(functions[:3])
            desc = f"Provides {func_list} functionality for {language}"
        else:
            # Generic description
            lines = len(code.split('\n'))
            desc = f"{language.capitalize()} block with {lines} lines of code implementing {name}"

        return desc

    def _generate_short_desc(self, description: str, name: str) -> str:
        """Generate brief one-line summary"""
        # Take first sentence or first 80 chars
        first_sentence = description.split('.')[0]
        if len(first_sentence) <= 80:
            return first_sentence
        return description[:77] + "..."

    def _extract_types(self, code: str, language: str) -> tuple:
        """Extract input and output types from code"""
        input_types = "any"
        output_type = "void"

        if language == 'cpp':
            # Look for function signature
            match = re.search(r'([\w:]+)\s+\w+\s*\((.*?)\)', code)
            if match:
                output_type = match.group(1)
                params = match.group(2)
                if params.strip():
                    # Parse parameter types
                    param_types = re.findall(r'([\w:]+)\s+\w+', params)
                    input_types = ', '.join(param_types) if param_types else "any"

        elif language == 'javascript':
            # Check for TypeScript-style annotations
            if '->' in code or ': ' in code:
                input_types = "typed"
                output_type = "typed"
            else:
                input_types = "any"
                output_type = "any"

        elif language == 'python':
            # Look for type hints
            match = re.search(r'def\s+\w+\s*\((.*?)\)\s*->\s*(\w+)', code)
            if match:
                params = match.group(1)
                output_type = match.group(2)
                if ':' in params:
                    param_types = re.findall(r':\s*([\w\[\]]+)', params)
                    input_types = ', '.join(param_types) if param_types else "any"

        return input_types, output_type

    def _extract_keywords(self, code: str, name: str, language: str) -> List[str]:
        """Extract relevant keywords from code and name"""
        keywords = set()

        # Add language
        keywords.add(language)

        # Extract from name (split camelCase and snake_case)
        name_words = re.findall(r'[A-Z][a-z]+|[a-z]+', name)
        keywords.update(w.lower() for w in name_words if len(w) > 2)

        # Extract from code (common programming patterns)
        common_patterns = [
            'parse', 'format', 'convert', 'validate', 'transform',
            'create', 'delete', 'update', 'read', 'write',
            'sort', 'filter', 'map', 'reduce', 'find',
            'http', 'json', 'xml', 'csv', 'api',
            'string', 'array', 'object', 'number', 'boolean',
            'async', 'promise', 'callback', 'event',
        ]

        code_lower = code.lower()
        for pattern in common_patterns:
            if pattern in code_lower:
                keywords.add(pattern)

        # Limit to top 10 most relevant
        return sorted(list(keywords))[:10]

    def _generate_use_cases(self, code: str, name: str, language: str) -> List[str]:
        """Generate example use cases from code analysis"""
        use_cases = []

        # Infer from function names
        if 'parse' in name.lower():
            use_cases.append(f"Parsing {language} data structures")
        if 'validate' in name.lower():
            use_cases.append("Input validation and verification")
        if 'format' in name.lower():
            use_cases.append("Data formatting and transformation")
        if 'http' in code.lower() or 'request' in code.lower():
            use_cases.append("HTTP requests and API integration")
        if 'json' in code.lower():
            use_cases.append("JSON data processing")
        if 'file' in code.lower() or 'read' in code.lower():
            use_cases.append("File I/O operations")

        # Default use case
        if not use_cases:
            use_cases.append(f"General {language} programming tasks")

        return use_cases[:5]  # Limit to 5 use cases

    def _find_related_blocks(self, block_id: str, keywords: List[str], language: str) -> List[str]:
        """Find related blocks (placeholder - requires database)"""
        # In a full implementation, this would search the database
        # For now, return empty list
        return []

    def _estimate_performance(self, code: str, language: str) -> Dict[str, Any]:
        """Estimate performance metrics from code complexity"""
        lines = len(code.split('\n'))
        loops = len(re.findall(r'\b(for|while|forEach|map)\b', code))
        recursion = 'recursive' in code.lower() or code.count('return') > 5

        # Simple heuristic-based estimation
        if lines < 20 and loops < 2:
            tier = "fast"
            avg_ms = 1.0
            memory_mb = 1
        elif lines < 100 and loops < 5:
            tier = "medium"
            avg_ms = 10.0
            memory_mb = 4
        else:
            tier = "slow"
            avg_ms = 50.0
            memory_mb = 16

        if recursion:
            avg_ms *= 2
            memory_mb *= 2

        return {
            'avg_execution_ms': avg_ms,
            'max_memory_mb': memory_mb,
            'performance_tier': tier,
        }

    def _default_quality_metrics(self) -> Dict[str, Any]:
        """Provide default quality metrics"""
        return {
            'success_rate_percent': 100,  # Assume stable until proven otherwise
            'avg_tokens_saved': 50,       # Conservative estimate
            'test_coverage_percent': 0,   # Unknown until tested
            'security_audited': False,    # Not audited by default
            'stability': 'stable',        # Assume stable
        }

    def _extract_function_names(self, code: str, language: str) -> List[str]:
        """Extract function names from code"""
        functions = []
        patterns = self.language_patterns.get(language, {})
        func_pattern = patterns.get('function', '')

        if func_pattern:
            matches = re.findall(func_pattern, code)
            for match in matches:
                if isinstance(match, tuple):
                    functions.extend(f for f in match if f)
                else:
                    functions.append(match)

        return functions[:5]  # Limit to first 5

    def _extract_comments(self, code: str, language: str) -> List[str]:
        """Extract comments from code"""
        comments = []

        if language in ['cpp', 'javascript']:
            # Single-line comments
            comments.extend(re.findall(r'//\s*(.+)', code))
            # Multi-line comments
            comments.extend(re.findall(r'/\*\s*(.+?)\s*\*/', code, re.DOTALL))

        elif language == 'python':
            # Python comments and docstrings
            comments.extend(re.findall(r'#\s*(.+)', code))
            comments.extend(re.findall(r'"""(.+?)"""', code, re.DOTALL))
            comments.extend(re.findall(r"'''(.+?)'''", code, re.DOTALL))

        return [c.strip() for c in comments if c.strip()]


def enrich_file(file_path: Path, enricher: BlockMetadataEnricher, backup: bool = True) -> bool:
    """
    Enrich a single block file

    Args:
        file_path: Path to block JSON file
        enricher: Enricher instance
        backup: Whether to create backup before modification

    Returns:
        True if successful, False otherwise
    """
    try:
        with open(file_path, 'r') as f:
            block_data = json.load(f)

        # Check if already enriched
        if 'description' in block_data and block_data.get('description'):
            print(f"  ⊙ {file_path.name} - already enriched, skipping")
            return True

        # Enrich the block
        enriched_data = enricher.enrich_block(block_data)

        # Backup original file
        if backup:
            backup_path = file_path.with_suffix('.json.bak')
            if not backup_path.exists():
                with open(backup_path, 'w') as f:
                    json.dump(block_data, f, indent=2)

        # Write enriched data
        with open(file_path, 'w') as f:
            json.dump(enriched_data, f, indent=2)

        print(f"  ✓ {file_path.name} - enriched successfully")
        return True

    except Exception as e:
        print(f"  ✗ {file_path.name} - ERROR: {e}")
        return False


def main():
    """Main entry point"""
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    enricher = BlockMetadataEnricher()

    # Handle different modes
    if sys.argv[1] == '--single':
        # Enrich single file
        if len(sys.argv) < 3:
            print("Error: --single requires a file path")
            sys.exit(1)

        file_path = Path(sys.argv[2])
        if not file_path.exists():
            print(f"Error: File not found: {file_path}")
            sys.exit(1)

        print(f"Enriching single file: {file_path}")
        success = enrich_file(file_path, enricher)
        sys.exit(0 if success else 1)

    elif sys.argv[1] == '--test':
        # Test mode - enrich random sample
        count = int(sys.argv[2]) if len(sys.argv) > 2 else 100
        blocks_dir = Path('/storage/emulated/0/Download/.naab/naab/blocks/library')

        print(f"Test mode: Enriching {count} random blocks from {blocks_dir}")

        # Find all JSON files
        all_files = list(blocks_dir.rglob('*.json'))
        if not all_files:
            print(f"Error: No JSON files found in {blocks_dir}")
            sys.exit(1)

        # Random sample
        sample_files = random.sample(all_files, min(count, len(all_files)))

        success_count = 0
        for file_path in sample_files:
            if enrich_file(file_path, enricher):
                success_count += 1

        print(f"\n✓ Enriched {success_count}/{len(sample_files)} blocks successfully")
        sys.exit(0 if success_count == len(sample_files) else 1)

    else:
        # Enrich entire directory
        blocks_dir = Path(sys.argv[1])
        if not blocks_dir.exists():
            print(f"Error: Directory not found: {blocks_dir}")
            sys.exit(1)

        print(f"Enriching all blocks in: {blocks_dir}")
        print("=" * 80)

        # Find all JSON files recursively
        json_files = list(blocks_dir.rglob('*.json'))
        total = len(json_files)

        print(f"Found {total} JSON files")
        print()

        success_count = 0
        for i, file_path in enumerate(json_files, 1):
            print(f"[{i}/{total}] ", end='')
            if enrich_file(file_path, enricher):
                success_count += 1

        print()
        print("=" * 80)
        print(f"✓ Enrichment complete: {success_count}/{total} blocks enriched")

        sys.exit(0 if success_count == total else 1)


if __name__ == '__main__':
    main()

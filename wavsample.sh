find data -type f -name "*.wav" | while read -r file; do
    samples=$(soxi -s "$file" 2>/dev/null)
    
    # Check if samples is a valid number and greater than 65536
    if [ -n "$samples" ] && [ "$samples" -gt 65536 ]; then
        echo "❌ OVER LIMIT ($samples): $file"
    else
        echo "✅ OK ($samples): $file"
    fi
done

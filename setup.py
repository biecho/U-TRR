from setuptools import setup, find_packages

with open("requirements.txt", "r") as f:
    requirements = f.read().splitlines()

setup(
    name="u_trr_safari",  # Replace with your package name
    version="0.1.0",  # Adjust version as needed
    packages=find_packages(),  # Automatically find package directories
    install_requires=requirements,  # Use the parsed requirements
)
